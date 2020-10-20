#include <boost/test/unit_test.hpp>

#include <muse/chain/database.hpp>
#include <muse/chain/exceptions.hpp>
#include <muse/chain/hardfork.hpp>
#include <muse/chain/history_object.hpp>
#include <muse/chain/base_objects.hpp>

#include <fc/crypto/digest.hpp>

#include "../common/database_fixture.hpp"

#include <cmath>

using namespace muse::chain;
using namespace muse::chain::test;

BOOST_FIXTURE_TEST_SUITE( operation_time_tests, clean_database_fixture )

BOOST_AUTO_TEST_CASE( vesting_withdrawals )
{
   try
   {
      ACTORS( (alice) )
      fund( "alice", 100000 );
      vest( "alice", 100000 );

      const auto& new_alice = db.get_account( "alice" );

      BOOST_TEST_MESSAGE( "Setting up withdrawal" );

      signed_transaction tx;
      withdraw_vesting_operation op;
      op.account = "alice";
      op.vesting_shares = asset( new_alice.vesting_shares.amount / 2, VESTS_SYMBOL );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      auto next_withdrawal = db.head_block_time() + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS;
      asset vesting_shares = new_alice.vesting_shares;
      asset original_vesting = vesting_shares;
      asset withdraw_rate = new_alice.vesting_withdraw_rate;

      BOOST_TEST_MESSAGE( "Generating block up to first withdrawal" );
      generate_blocks( next_withdrawal - ( MUSE_BLOCK_INTERVAL / 2 ), true);

      BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).vesting_shares.amount.value, vesting_shares.amount.value );

      BOOST_TEST_MESSAGE( "Generating block to cause withdrawal" );
      generate_block();

      auto fill_op = get_last_operations( 1 )[0].get< fill_vesting_withdraw_operation >();
      auto gpo = db.get_dynamic_global_properties();

      BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).vesting_shares.amount.value, ( vesting_shares - withdraw_rate ).amount.value );
      BOOST_REQUIRE( ( withdraw_rate * gpo.get_vesting_share_price() ).amount.value - db.get_account( "alice" ).balance.amount.value <= 1 ); // Check a range due to differences in the share price
      BOOST_REQUIRE_EQUAL( fill_op.from_account, "alice" );
      BOOST_REQUIRE_EQUAL( fill_op.to_account, "alice" );
      BOOST_REQUIRE_EQUAL( fill_op.withdrawn.amount.value, withdraw_rate.amount.value );
      BOOST_REQUIRE( std::abs( ( fill_op.deposited - fill_op.withdrawn * gpo.get_vesting_share_price() ).amount.value ) <= 1 );
      validate_database();

      BOOST_TEST_MESSAGE( "Generating the rest of the blocks in the withdrawal" );

      vesting_shares = db.get_account( "alice" ).vesting_shares;
      auto balance = db.get_account( "alice" ).balance;
      auto old_next_vesting = db.get_account( "alice" ).next_vesting_withdrawal;

      for( int i = 1; i < MUSE_VESTING_WITHDRAW_INTERVALS - 1; i++ )
      {
         generate_blocks( db.head_block_time() + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS );

         const auto& alice = db.get_account( "alice" );

         gpo = db.get_dynamic_global_properties();
         fill_op = get_last_operations( 1 )[0].get< fill_vesting_withdraw_operation >();

         BOOST_REQUIRE_EQUAL( alice.vesting_shares.amount.value, ( vesting_shares - withdraw_rate ).amount.value );
         BOOST_REQUIRE( balance.amount.value + ( withdraw_rate * gpo.get_vesting_share_price() ).amount.value - alice.balance.amount.value <= 1 );
         BOOST_REQUIRE_EQUAL( fill_op.from_account, "alice" );
         BOOST_REQUIRE_EQUAL( fill_op.to_account, "alice" );
         BOOST_REQUIRE_EQUAL( fill_op.withdrawn.amount.value, withdraw_rate.amount.value );
         BOOST_REQUIRE( std::abs( ( fill_op.deposited - fill_op.withdrawn * gpo.get_vesting_share_price() ).amount.value ) <= 1 );

         if ( i == MUSE_VESTING_WITHDRAW_INTERVALS - 1 )
            BOOST_REQUIRE( alice.next_vesting_withdrawal == fc::time_point_sec::maximum() );
         else
            BOOST_REQUIRE_EQUAL( alice.next_vesting_withdrawal.sec_since_epoch(), ( old_next_vesting + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS ).sec_since_epoch() );

         validate_database();

         vesting_shares = alice.vesting_shares;
         balance = alice.balance;
         old_next_vesting = alice.next_vesting_withdrawal;
      }

      if (  original_vesting.amount.value % withdraw_rate.amount.value != 0 )
      {
         BOOST_TEST_MESSAGE( "Generating one more block to take care of remainder" );
         generate_blocks( db.head_block_time() + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS, true );
         fill_op = get_last_operations( 1 )[0].get< fill_vesting_withdraw_operation >();

         BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).next_vesting_withdrawal.sec_since_epoch(), ( old_next_vesting + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS ).sec_since_epoch() );
         BOOST_REQUIRE_EQUAL( fill_op.from_account, "alice" );
         BOOST_REQUIRE_EQUAL( fill_op.to_account, "alice" );
         BOOST_REQUIRE_EQUAL( fill_op.withdrawn.amount.value, withdraw_rate.amount.value );
         BOOST_REQUIRE( std::abs( ( fill_op.deposited - fill_op.withdrawn * gpo.get_vesting_share_price() ).amount.value ) <= 1 );

         generate_blocks( db.head_block_time() + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS, true );
         fill_op = get_last_operations( 1 )[0].get< fill_vesting_withdraw_operation >();

         BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).next_vesting_withdrawal.sec_since_epoch(), ( old_next_vesting + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS ).sec_since_epoch() );
         BOOST_REQUIRE_EQUAL( fill_op.to_account, "alice" );
         BOOST_REQUIRE_EQUAL( fill_op.from_account, "alice" );
         BOOST_REQUIRE_EQUAL( fill_op.withdrawn.amount.value, original_vesting.amount.value % withdraw_rate.amount.value );
         BOOST_REQUIRE( std::abs( ( fill_op.deposited - fill_op.withdrawn * gpo.get_vesting_share_price() ).amount.value ) <= 1 );

         validate_database();
      }
      else
      {
         generate_blocks( db.head_block_time() + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS, true );

         BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).next_vesting_withdrawal.sec_since_epoch(), fc::time_point_sec::maximum().sec_since_epoch() );

         fill_op = get_last_operations( 1 )[0].get< fill_vesting_withdraw_operation >();
         BOOST_REQUIRE_EQUAL( fill_op.from_account, "alice" );
         BOOST_REQUIRE_EQUAL( fill_op.to_account, "alice" );
         BOOST_REQUIRE_EQUAL( fill_op.withdrawn.amount.value, withdraw_rate.amount.value );
         BOOST_REQUIRE( std::abs( ( fill_op.deposited - fill_op.withdrawn * gpo.get_vesting_share_price() ).amount.value ) <= 1 );
      }

      BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).vesting_shares.amount.value, ( original_vesting - op.vesting_shares ).amount.value );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( vesting_withdraw_route )
{
   try
   {
      ACTORS( (alice)(bob)(sam) )

      auto original_vesting = alice.vesting_shares;

      fund( "alice", 1040000 );
      vest( "alice", 1040000 );

      auto withdraw_amount = alice.vesting_shares - original_vesting;

      BOOST_TEST_MESSAGE( "Setup vesting withdraw" );
      withdraw_vesting_operation wv;
      wv.account = "alice";
      wv.vesting_shares = withdraw_amount;

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( wv );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      tx.operations.clear();
      tx.signatures.clear();

      BOOST_TEST_MESSAGE( "Setting up bob destination" );
      set_withdraw_vesting_route_operation op;
      op.from_account = "alice";
      op.to_account = "bob";
      op.percent = MUSE_1_PERCENT * 50;
      op.auto_vest = true;
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "Setting up sam destination" );
      op.to_account = "sam";
      op.percent = MUSE_1_PERCENT * 30;
      op.auto_vest = false;
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_TEST_MESSAGE( "Setting up first withdraw" );

      auto vesting_withdraw_rate = alice.vesting_withdraw_rate;
      auto old_alice_balance = alice.balance;
      auto old_alice_vesting = alice.vesting_shares;
      auto old_bob_balance = bob.balance;
      auto old_bob_vesting = bob.vesting_shares;
      auto old_sam_balance = sam.balance;
      auto old_sam_vesting = sam.vesting_shares;
      generate_blocks( alice.next_vesting_withdrawal, true );

      {
         const auto& alice = db.get_account( "alice" );
         const auto& bob = db.get_account( "bob" );
         const auto& sam = db.get_account( "sam" );

         BOOST_REQUIRE( alice.vesting_shares == old_alice_vesting - vesting_withdraw_rate );
         BOOST_REQUIRE( alice.balance == old_alice_balance + asset( ( vesting_withdraw_rate.amount * MUSE_1_PERCENT * 20 ) / MUSE_100_PERCENT, VESTS_SYMBOL ) * db.get_dynamic_global_properties().get_vesting_share_price() );
         BOOST_REQUIRE( bob.vesting_shares == old_bob_vesting + asset( ( vesting_withdraw_rate.amount * MUSE_1_PERCENT * 50 ) / MUSE_100_PERCENT, VESTS_SYMBOL ) );
         BOOST_REQUIRE( bob.balance == old_bob_balance );
         BOOST_REQUIRE( sam.vesting_shares == old_sam_vesting );
         BOOST_REQUIRE( sam.balance ==  old_sam_balance + asset( ( vesting_withdraw_rate.amount * MUSE_1_PERCENT * 30 ) / MUSE_100_PERCENT, VESTS_SYMBOL ) * db.get_dynamic_global_properties().get_vesting_share_price() );

         old_alice_balance = alice.balance;
         old_alice_vesting = alice.vesting_shares;
         old_bob_balance = bob.balance;
         old_bob_vesting = bob.vesting_shares;
         old_sam_balance = sam.balance;
         old_sam_vesting = sam.vesting_shares;
      }

      BOOST_TEST_MESSAGE( "Test failure with greater than 100% destination assignment" );

      tx.operations.clear();
      tx.signatures.clear();

      op.to_account = "sam";
      op.percent = MUSE_1_PERCENT * 50 + 1;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_TEST_MESSAGE( "Test from_account receiving no withdraw" );

      tx.operations.clear();
      tx.signatures.clear();

      op.to_account = "sam";
      op.percent = MUSE_1_PERCENT * 50;
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      generate_blocks( db.get_account( "alice" ).next_vesting_withdrawal, true );
      {
         const auto& alice = db.get_account( "alice" );
         const auto& bob = db.get_account( "bob" );
         const auto& sam = db.get_account( "sam" );

         BOOST_REQUIRE( alice.vesting_shares == old_alice_vesting - vesting_withdraw_rate );
         BOOST_REQUIRE( alice.balance == old_alice_balance );
         BOOST_REQUIRE( bob.vesting_shares == old_bob_vesting + asset( ( vesting_withdraw_rate.amount * MUSE_1_PERCENT * 50 ) / MUSE_100_PERCENT, VESTS_SYMBOL ) );
         BOOST_REQUIRE( bob.balance == old_bob_balance );
         BOOST_REQUIRE( sam.vesting_shares == old_sam_vesting );
         BOOST_REQUIRE( sam.balance ==  old_sam_balance + asset( ( vesting_withdraw_rate.amount * MUSE_1_PERCENT * 50 ) / MUSE_100_PERCENT, VESTS_SYMBOL ) * db.get_dynamic_global_properties().get_vesting_share_price() );
      }
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( feed_publish_mean )
{
   try
   {
      ACTORS( (alice0)(alice1)(alice2)(alice3)(alice4)(alice5)(alice6) )

      BOOST_TEST_MESSAGE( "Setup" );

      generate_blocks( 30 / MUSE_BLOCK_INTERVAL );

      vector< string > accounts;
      accounts.push_back( "alice0" );
      accounts.push_back( "alice1" );
      accounts.push_back( "alice2" );
      accounts.push_back( "alice3" );
      accounts.push_back( "alice4" );
      accounts.push_back( "alice5" );
      accounts.push_back( "alice6" );

      vector< private_key_type > keys;
      keys.push_back( alice0_private_key );
      keys.push_back( alice1_private_key );
      keys.push_back( alice2_private_key );
      keys.push_back( alice3_private_key );
      keys.push_back( alice4_private_key );
      keys.push_back( alice5_private_key );
      keys.push_back( alice6_private_key );

      vector< feed_publish_operation > ops;
      vector< signed_transaction > txs;

      // Upgrade accounts to witnesses
      for( int i = 0; i < 7; i++ )
      {
         transfer( MUSE_INIT_MINER_NAME, accounts[i], 10000 );
         witness_create( accounts[i], keys[i], "foo.bar", keys[i].get_public_key(), 1000 );

         ops.push_back( feed_publish_operation() );
         ops[i].publisher = accounts[i];

         txs.push_back( signed_transaction() );
      }

      ops[0].exchange_rate = price( asset( 100000, MUSE_SYMBOL ), asset( 1000, MBD_SYMBOL ) );
      ops[1].exchange_rate = price( asset( 105000, MUSE_SYMBOL ), asset( 1000, MBD_SYMBOL ) );
      ops[2].exchange_rate = price( asset(  98000, MUSE_SYMBOL ), asset( 1000, MBD_SYMBOL ) );
      ops[3].exchange_rate = price( asset(  97000, MUSE_SYMBOL ), asset( 1000, MBD_SYMBOL ) );
      ops[4].exchange_rate = price( asset(  99000, MUSE_SYMBOL ), asset( 1000, MBD_SYMBOL ) );
      ops[5].exchange_rate = price( asset(  97500, MUSE_SYMBOL ), asset( 1000, MBD_SYMBOL ) );
      ops[6].exchange_rate = price( asset( 102000, MUSE_SYMBOL ), asset( 1000, MBD_SYMBOL ) );

      for( int i = 0; i < 7; i++ )
      {
         txs[i].set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
         txs[i].operations.push_back( ops[i] );
         txs[i].sign( keys[i], db.get_chain_id() );
         db.push_transaction( txs[i], 0 );
      }

      BOOST_TEST_MESSAGE( "Jump forward an hour" );

      generate_blocks( MUSE_BLOCKS_PER_HOUR ); // Jump forward 1 hour
      BOOST_TEST_MESSAGE( "Get feed history object" );
      feed_history_object feed_history = db.get_feed_history();
      BOOST_TEST_MESSAGE( "Check state" );
      BOOST_REQUIRE( feed_history.actual_median_history == price( asset( 99000, MUSE_SYMBOL), asset( 1000, MBD_SYMBOL ) ) );
      BOOST_REQUIRE( feed_history.effective_median_history == price( asset( 99000, MUSE_SYMBOL), asset( 1000, MBD_SYMBOL ) ) );
      BOOST_REQUIRE( feed_history.price_history[ 0 ] == price( asset( 99000, MUSE_SYMBOL), asset( 1000, MBD_SYMBOL ) ) );
      validate_database();

      for ( int i = 0; i < 23; i++ )
      {
         BOOST_TEST_MESSAGE( "Updating ops" );

         for( int j = 0; j < 7; j++ )
         {
            txs[j].operations.clear();
            txs[j].signatures.clear();
            ops[j].exchange_rate = price( ops[j].exchange_rate.base, asset( ops[j].exchange_rate.quote.amount + 10, MBD_SYMBOL ) );
            txs[j].set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
            txs[j].operations.push_back( ops[j] );
            txs[j].sign( keys[j], db.get_chain_id() );
            db.push_transaction( txs[j], 0 );
         }

         BOOST_TEST_MESSAGE( "Generate Blocks" );

         generate_blocks( MUSE_BLOCKS_PER_HOUR  ); // Jump forward 1 hour

         BOOST_TEST_MESSAGE( "Check feed_history" );

         feed_history = feed_history_id_type()( db );
         BOOST_REQUIRE( feed_history.actual_median_history == feed_history.price_history[ ( i + 1 ) / 2 ] );
         BOOST_REQUIRE( feed_history.effective_median_history == feed_history.price_history[ ( i + 1 ) / 2 ] );
         BOOST_REQUIRE( feed_history.price_history[ i + 1 ] == ops[4].exchange_rate );
         validate_database();
      }
   }
   FC_LOG_AND_RETHROW();
}

BOOST_AUTO_TEST_CASE( convert_delay )
{
   try
   {
      ACTORS( (alice) )

      set_price_feed( price( asset::from_string( "1.250 2.28.0" ), asset::from_string( "1.000 2.28.2" ) ) );
/*
      convert_operation op;
      //comment_operation comment;
      vote_operation vote;
      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      //comment.author = "alice";
      //comment.title = "foo";
      //comment.body = "bar";
      //comment.permlink = "test";
      //comment.parent_permlink = "test";
      //tx.operations.push_back( comment );
      //tx.sign( alice_private_key, db.get_chain_id() );
      //db.push_transaction( tx, 0 );

      //tx.operations.clear();
      //tx.signatures.clear();
      //vote.voter = "alice";
      //vote.author = "alice";
      //vote.permlink = "test";
      //vote.weight = MUSE_100_PERCENT;
      //tx.operations.push_back( vote );
      //tx.sign( alice_private_key, db.get_chain_id() );
      //db.push_transaction( tx, 0 );

      //generate_blocks( db.get_comment( "alice", "test" ).cashout_time, true );

      auto start_balance = asset( db.get_comment( "alice", "test" ).total_payout_value.amount / 2, MBD_SYMBOL );

      BOOST_TEST_MESSAGE( "Setup conversion to TESTS" );
      tx.operations.clear();
      tx.signatures.clear();
      op.owner = "alice";
      op.amount = asset( 2000000, MBD_SYMBOL );
      op.requestid = 2;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_TEST_MESSAGE( "Generating Blocks up to conversion block" );
      generate_blocks( db.head_block_time() + MUSE_CONVERSION_DELAY - fc::seconds( MUSE_BLOCK_INTERVAL / 2 ), true );

      BOOST_TEST_MESSAGE( "Verify conversion is not applied" );
      const auto& alice_2 = db.get_account( "alice" );
      const auto& convert_request_idx = db.get_index_type< convert_index >().indices().get< by_owner >();
      auto convert_request = convert_request_idx.find( std::make_tuple( "alice", 2 ) );

      BOOST_REQUIRE( convert_request != convert_request_idx.end() );
      BOOST_REQUIRE_EQUAL( alice_2.balance.amount.value, 0 );
      BOOST_REQUIRE_EQUAL( alice_2.mbd_balance.amount.value, ( start_balance - op.amount ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "Generate one more block" );
      generate_block();

      BOOST_TEST_MESSAGE( "Verify conversion applied" );
      const auto& alice_3 = db.get_account( "alice" );
      auto vop = get_last_operations( 1 )[0].get< fill_convert_request_operation >();

      convert_request = convert_request_idx.find( std::make_tuple( "alice", 2 ) );
      BOOST_REQUIRE( convert_request == convert_request_idx.end() );
      BOOST_REQUIRE_EQUAL( alice_3.balance.amount.value, 2500000 );
      BOOST_REQUIRE_EQUAL( alice_3.mbd_balance.amount.value, ( start_balance - op.amount ).amount.value );
      BOOST_REQUIRE_EQUAL( vop.owner, "alice" );
      BOOST_REQUIRE_EQUAL( vop.requestid, 2 );
      BOOST_REQUIRE_EQUAL( vop.amount_in.amount.value, ASSET( "2.000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( vop.amount_out.amount.value, ASSET( "2.500 2.28.0" ).amount.value );
  */
      validate_database();
   }
   FC_LOG_AND_RETHROW();
}

BOOST_AUTO_TEST_CASE( muse_inflation )
{
   try
   {
   /*
      BOOST_TEST_MESSAGE( "Testing MUSE Inflation until the vesting start block" );

      auto gpo = db.get_dynamic_global_properties();
      auto virtual_supply = gpo.virtual_supply;
      auto witness_name = db.get_scheduled_witness(1);
      auto old_witness_balance = db.get_account( witness_name ).balance;
      auto old_witness_shares = db.get_account( witness_name ).vesting_shares;

      auto new_rewards = std::max( MUSE_MIN_CONTENT_REWARD, asset( ( MUSE_CONTENT_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) )
         + std::max( MUSE_MIN_CURATE_REWARD, asset( ( MUSE_CURATE_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) );
      auto witness_pay = std::max( MUSE_MIN_PRODUCER_REWARD, asset( ( MUSE_PRODUCER_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) );
      auto witness_pay_shares = asset( 0, VESTS_SYMBOL );
      auto new_vesting_muse = asset( 0, MUSE_SYMBOL );
      auto new_vesting_shares = gpo.total_vesting_shares;

      if ( db.get_account( witness_name ).vesting_shares.amount.value == 0 )
      {
         new_vesting_muse += witness_pay;
         new_vesting_shares += witness_pay * ( gpo.total_vesting_shares / gpo.total_vesting_fund_muse );
      }

      auto new_supply = gpo.current_supply + new_rewards + witness_pay + new_vesting_muse;
      new_rewards += gpo.total_reward_fund_muse;
      new_vesting_muse += gpo.total_vesting_fund_muse;

      generate_block();

      gpo = db.get_dynamic_global_properties();

      BOOST_REQUIRE_EQUAL( gpo.current_supply.amount.value, new_supply.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.virtual_supply.amount.value, new_supply.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_reward_fund_muse.amount.value, new_rewards.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_vesting_fund_muse.amount.value, new_vesting_muse.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_vesting_shares.amount.value, new_vesting_shares.amount.value );
      BOOST_REQUIRE_EQUAL( db.get_account( witness_name ).balance.amount.value, ( old_witness_balance + witness_pay ).amount.value );

      validate_database();

      while( db.head_block_num() < MUSE_START_VESTING_BLOCK - 1)
      {
         virtual_supply = gpo.virtual_supply;
         witness_name = db.get_scheduled_witness(1);
         old_witness_balance = db.get_account( witness_name ).balance;
         old_witness_shares = db.get_account( witness_name ).vesting_shares;


         new_rewards = std::max( MUSE_MIN_CONTENT_REWARD, asset( ( MUSE_CONTENT_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) )
            + std::max( MUSE_MIN_CURATE_REWARD, asset( ( MUSE_CURATE_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) );
         witness_pay = std::max( MUSE_MIN_PRODUCER_REWARD, asset( ( MUSE_PRODUCER_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) );
         new_vesting_muse = asset( 0, MUSE_SYMBOL );
         new_vesting_shares = gpo.total_vesting_shares;

         if ( db.get_account( witness_name ).vesting_shares.amount.value == 0 )
         {
            new_vesting_muse += witness_pay;
            witness_pay_shares = witness_pay * gpo.get_vesting_share_price();
            new_vesting_shares += witness_pay_shares;
            new_supply += witness_pay;
            witness_pay = asset( 0, MUSE_SYMBOL );
         }

         new_supply = gpo.current_supply + new_rewards + witness_pay + new_vesting_muse;
         new_rewards += gpo.total_reward_fund_muse;
         new_vesting_muse += gpo.total_vesting_fund_muse;

         generate_block();

         gpo = db.get_dynamic_global_properties();

         BOOST_REQUIRE_EQUAL( gpo.current_supply.amount.value, new_supply.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.virtual_supply.amount.value, new_supply.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_reward_fund_muse.amount.value, new_rewards.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_vesting_fund_muse.amount.value, new_vesting_muse.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_vesting_shares.amount.value, new_vesting_shares.amount.value );
         BOOST_REQUIRE_EQUAL( db.get_account( witness_name ).balance.amount.value, ( old_witness_balance + witness_pay ).amount.value );
         BOOST_REQUIRE_EQUAL( db.get_account( witness_name ).vesting_shares.amount.value, ( old_witness_shares + witness_pay_shares ).amount.value );

         validate_database();
      }

      BOOST_TEST_MESSAGE( "Testing up to the start block for miner voting" );

      while( db.head_block_num() < MUSE_START_MINER_VOTING_BLOCK - 1 )
      {
         virtual_supply = gpo.virtual_supply;
         witness_name = db.get_scheduled_witness(1);
         old_witness_balance = db.get_account( witness_name ).balance;

         new_rewards = std::max( MUSE_MIN_CONTENT_REWARD, asset( ( MUSE_CONTENT_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) )
            + std::max( MUSE_MIN_CURATE_REWARD, asset( ( MUSE_CURATE_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) );
         witness_pay = std::max( MUSE_MIN_PRODUCER_REWARD, asset( ( MUSE_PRODUCER_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) );
         auto witness_pay_shares = asset( 0, VESTS_SYMBOL );
         new_vesting_muse = asset( ( witness_pay + new_rewards ).amount * 9, MUSE_SYMBOL );
         new_vesting_shares = gpo.total_vesting_shares;

         if ( db.get_account( witness_name ).vesting_shares.amount.value == 0 )
         {
            new_vesting_muse += witness_pay;
            witness_pay_shares = witness_pay * gpo.get_vesting_share_price();
            new_vesting_shares += witness_pay_shares;
            new_supply += witness_pay;
            witness_pay = asset( 0, MUSE_SYMBOL );
         }

         new_supply = gpo.current_supply + new_rewards + witness_pay + new_vesting_muse;
         new_rewards += gpo.total_reward_fund_muse;
         new_vesting_muse += gpo.total_vesting_fund_muse;

         generate_block();

         gpo = db.get_dynamic_global_properties();

         BOOST_REQUIRE_EQUAL( gpo.current_supply.amount.value, new_supply.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.virtual_supply.amount.value, new_supply.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_reward_fund_muse.amount.value, new_rewards.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_vesting_fund_muse.amount.value, new_vesting_muse.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_vesting_shares.amount.value, new_vesting_shares.amount.value );
         BOOST_REQUIRE_EQUAL( db.get_account( witness_name ).balance.amount.value, ( old_witness_balance + witness_pay ).amount.value );
         BOOST_REQUIRE_EQUAL( db.get_account( witness_name ).vesting_shares.amount.value, ( old_witness_shares + witness_pay_shares ).amount.value );

         validate_database();
      }

      for( int i = 0; i < MUSE_BLOCKS_PER_DAY; i++ )
      {
         virtual_supply = gpo.virtual_supply;
         witness_name = db.get_scheduled_witness(1);
         old_witness_balance = db.get_account( witness_name ).balance;

         new_rewards = std::max( MUSE_MIN_CONTENT_REWARD, asset( ( MUSE_CONTENT_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) )
            + std::max( MUSE_MIN_CURATE_REWARD, asset( ( MUSE_CURATE_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) );
         witness_pay = std::max( MUSE_MIN_PRODUCER_REWARD, asset( ( MUSE_PRODUCER_APR * gpo.virtual_supply.amount ) / ( MUSE_BLOCKS_PER_YEAR * 100 ), MUSE_SYMBOL ) );
         witness_pay_shares = witness_pay * gpo.get_vesting_share_price();
         new_vesting_muse = asset( ( witness_pay + new_rewards ).amount * 9, MUSE_SYMBOL ) + witness_pay;
         new_vesting_shares = gpo.total_vesting_shares + witness_pay_shares;
         new_supply = gpo.current_supply + new_rewards + new_vesting_muse;
         new_rewards += gpo.total_reward_fund_muse;
         new_vesting_muse += gpo.total_vesting_fund_muse;

         generate_block();

         gpo = db.get_dynamic_global_properties();

         BOOST_REQUIRE_EQUAL( gpo.current_supply.amount.value, new_supply.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.virtual_supply.amount.value, new_supply.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_reward_fund_muse.amount.value, new_rewards.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_vesting_fund_muse.amount.value, new_vesting_muse.amount.value );
         BOOST_REQUIRE_EQUAL( gpo.total_vesting_shares.amount.value, new_vesting_shares.amount.value );
         BOOST_REQUIRE_EQUAL( db.get_account( witness_name ).vesting_shares.amount.value, ( old_witness_shares + witness_pay_shares ).amount.value );

         validate_database();
      }
      virtual_supply = gpo.virtual_supply;
      vesting_shares = gpo.total_vesting_shares;
      vesting_muse = gpo.total_vesting_fund_muse;
      reward_muse = gpo.total_reward_fund_muse;

      witness_name = db.get_scheduled_witness(1);
      old_witness_shares = db.get_account( witness_name ).vesting_shares;

      generate_block();

      gpo = db.get_dynamic_global_properties();

      BOOST_REQUIRE_EQUAL( gpo.total_vesting_fund_muse.amount.value,
         ( vesting_muse.amount.value
            + ( ( ( uint128_t( virtual_supply.amount.value ) / 10 ) / MUSE_BLOCKS_PER_YEAR ) * 9 )
            + ( uint128_t( virtual_supply.amount.value ) / 100 / MUSE_BLOCKS_PER_YEAR ) ).to_uint64() );
      BOOST_REQUIRE_EQUAL( gpo.total_reward_fund_muse.amount.value,
         reward_muse.amount.value + virtual_supply.amount.value / 10 / MUSE_BLOCKS_PER_YEAR + virtual_supply.amount.value / 10 / MUSE_BLOCKS_PER_DAY );
      BOOST_REQUIRE_EQUAL( db.get_account( witness_name ).vesting_shares.amount.value,
         old_witness_shares.amount.value + ( asset( ( ( virtual_supply.amount.value / MUSE_BLOCKS_PER_YEAR ) * MUSE_1_PERCENT ) / MUSE_100_PERCENT, MUSE_SYMBOL ) * ( vesting_shares / vesting_muse ) ).amount.value );
      validate_database();
      */
   }
   FC_LOG_AND_RETHROW();
}

BOOST_AUTO_TEST_CASE( mbd_interest )
{
   try
   {
      ACTORS( (alice)(bob) )

      set_price_feed( price( asset::from_string( "1.000 2.28.0" ), asset::from_string( "1.000 2.28.2" ) ) );

      BOOST_TEST_MESSAGE( "Testing interest over smallest interest period" );
/*
      convert_operation op;
      comment_operation comment;
      vote_operation vote;
      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      comment.author = "alice";
      comment.title = "foo";
      comment.body = "bar";
      comment.permlink = "test";
      comment.parent_permlink = "test";
      tx.operations.push_back( comment );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      tx.operations.clear();
      tx.signatures.clear();
      vote.voter = "alice";
      vote.author = "alice";
      vote.permlink = "test";
      vote.weight = MUSE_100_PERCENT;
      tx.operations.push_back( vote );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      generate_blocks( db.get_comment( "alice", "test" ).cashout_time, true );

      auto start_time = db.get_account( "alice" ).mbd_seconds_last_update;
      auto alice_mbd = db.get_account( "alice" ).mbd_balance;

      generate_blocks( db.head_block_time() + fc::seconds( MUSE_SBD_INTEREST_COMPOUND_INTERVAL_SEC ), true );

      transfer_operation transfer;
      transfer.to = "bob";
      transfer.from = "alice";
      transfer.amount = ASSET( "1.000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( transfer );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      auto gpo = db.get_dynamic_global_properties();
      auto interest_op = get_last_operations( 1 )[0].get< interest_operation >();

      BOOST_REQUIRE( gpo.mbd_interest_rate > 0 );
      BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).mbd_balance.amount.value, alice_mbd.amount.value - ASSET( "1.000 TBD" ).amount.value + ( ( ( ( uint128_t( alice_mbd.amount.value ) * ( db.head_block_time() - start_time ).to_seconds() ) / MUSE_SECONDS_PER_YEAR ) * gpo.mbd_interest_rate ) / MUSE_100_PERCENT ).to_uint64() );
      BOOST_REQUIRE_EQUAL( interest_op.owner, "alice" );
      BOOST_REQUIRE_EQUAL( interest_op.interest.amount.value, db.get_account( "alice" ).mbd_balance.amount.value - ( alice_mbd.amount.value - ASSET( "1.000 TBD" ).amount.value ) );
      validate_database();

      BOOST_TEST_MESSAGE( "Testing interest under interest period" );

      start_time = db.get_account( "alice" ).mbd_seconds_last_update;
      alice_mbd = db.get_account( "alice" ).mbd_balance;

      generate_blocks( db.head_block_time() + fc::seconds( MUSE_SBD_INTEREST_COMPOUND_INTERVAL_SEC / 2 ), true );

      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( transfer );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).mbd_balance.amount.value, alice_mbd.amount.value - ASSET( "1.000 TBD" ).amount.value );
      validate_database();

      auto alice_coindays = uint128_t( alice_mbd.amount.value ) * ( db.head_block_time() - start_time ).to_seconds();
      alice_mbd = db.get_account( "alice" ).mbd_balance;
      start_time = db.get_account( "alice" ).mbd_seconds_last_update;

      BOOST_TEST_MESSAGE( "Testing longer interest period" );

      generate_blocks( db.head_block_time() + fc::seconds( ( MUSE_SBD_INTEREST_COMPOUND_INTERVAL_SEC * 7 ) / 3 ), true );

      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( transfer );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).mbd_balance.amount.value, alice_mbd.amount.value - ASSET( "1.000 TBD" ).amount.value + ( ( ( ( uint128_t( alice_mbd.amount.value ) * ( db.head_block_time() - start_time ).to_seconds() + alice_coindays ) / MUSE_SECONDS_PER_YEAR ) * gpo.mbd_interest_rate ) / MUSE_100_PERCENT ).to_uint64() );
*/
      validate_database();
   }
   FC_LOG_AND_RETHROW();
}

BOOST_AUTO_TEST_CASE( tx_rate_limit )
{ try {
   ACTORS( (alice)(bobby)(charlie) )

   fund( "alice", 10000000 );
   vest( "alice", 1000000 );
   fund( "bobby", 10000000 );
   fund( "charlie", 100000000 );
   vest( "charlie", 100000000 );

   generate_block();

   const dynamic_global_property_object& _dgp = dynamic_global_property_id_type(0)(db);
   db.modify( _dgp, []( dynamic_global_property_object& dgp ) {
      dgp.max_virtual_bandwidth = ( 5000 * MUSE_BANDWIDTH_PRECISION * MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS )
                                  / MUSE_BLOCK_INTERVAL;
   });

   signed_transaction tx;
   tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

   delegate_vesting_shares_operation dvs;
   dvs.delegator = "charlie";
   dvs.delegatee = "bobby";
   dvs.vesting_shares = asset( alice_id(db).vesting_shares.amount.value / 2 - bobby_id(db).vesting_shares.amount.value, VESTS_SYMBOL );
   tx.operations.push_back( dvs );
   tx.sign( charlie_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );
   tx.clear();

   transfer_operation op;
   op.from = "alice";
   op.to = "charlie";
   op.amount = asset( 1, MUSE_SYMBOL );
   op.memo = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
   for( int i = 0; i < 100; i++ )
      tx.operations.push_back( op );
   int alice_count = 0;
   try {
      while( alice_count++ < 100000 ) {
         tx.set_expiration( db.head_block_time() + 60 + (alice_count & 0x7ff) );
         db.push_transaction( tx, database::skip_transaction_signatures );
         if( !(alice_count & 0x7ff) )
         {
            op.amount.amount.value++;
            tx.operations[0] = op;
         }
      }
   } catch( fc::assert_exception& e ) {
       BOOST_REQUIRE( e.to_detail_string().find( "bandwidth" ) != string::npos );
       alice_count--;
   }
   tx.clear();
   BOOST_CHECK_LT( 10, alice_count );

   op.from = "bobby";
   op.amount = asset( 1, MUSE_SYMBOL );
   for( int i = 0; i < 100; i++ )
      tx.operations.push_back( op );
   int bobby_count = 0;
   try {
      while( bobby_count++ < 100000 ) {
         tx.set_expiration( db.head_block_time() + 60 + (bobby_count & 0x7ff) );
         db.push_transaction( tx, database::skip_transaction_signatures );
         if( !(bobby_count & 0x7ff) )
         {
            op.amount.amount.value++;
            tx.operations[0] = op;
         }
      }
   } catch( fc::assert_exception& e ) {
       BOOST_REQUIRE( e.to_detail_string().find( "bandwidth" ) != string::npos );
       bobby_count--;
   }
   tx.clear();

   // bobby has half as many VESTS as alice, so should have about half the bandwidth
   BOOST_CHECK_EQUAL( alice_count / 2, bobby_count );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
