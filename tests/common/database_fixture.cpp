#include <boost/test/unit_test.hpp>
#include <boost/program_options.hpp>

#include <muse/chain/protocol/ext.hpp>
#include <graphene/db/simple_index.hpp>
#include <graphene/utilities/tempdir.hpp>

#include <muse/chain/base_objects.hpp>
#include <muse/chain/history_object.hpp>
#include <muse/account_history/account_history_plugin.hpp>
#include <muse/custom_tags/custom_tags.hpp>

#include <fc/crypto/digest.hpp>
#include <fc/smart_ref_impl.hpp>

#include <iostream>
#include <iomanip>
#include <sstream>

#include "database_fixture.hpp"

using namespace muse::chain::test;

uint32_t MUSE_TESTING_GENESIS_TIMESTAMP = 1431700000;

namespace muse { namespace chain {

using std::cout;
using std::cerr;

clean_database_fixture::clean_database_fixture()
{
   initialize_clean( MUSE_NUM_HARDFORKS );
}

void database_fixture::initialize_clean( uint32_t num_hardforks )
{ try {
   int argc = boost::unit_test::framework::master_test_suite().argc;
   char** argv = boost::unit_test::framework::master_test_suite().argv;
   for( int i=1; i<argc; i++ )
   {
      const std::string arg = argv[i];
      if( arg == "--record-assert-trip" )
         fc::enable_record_assert_trip = true;
      if( arg == "--show-test-names" )
         std::cout << "running test " << boost::unit_test::framework::current_test_case().p_name << std::endl;
   }
   auto ahplugin = app.register_plugin< muse::account_history::account_history_plugin >();
   auto ctplugin = app.register_plugin< muse::custom_tags::custom_tags_plugin >();
   init_account_pub_key = init_account_priv_key.get_public_key();

   boost::program_options::variables_map options;

   open_database();

   // app.initialize();
   ahplugin->plugin_set_app( &app );
   ctplugin->plugin_set_app( &app );
   ahplugin->plugin_initialize( options );
   ctplugin->plugin_initialize( options );

   validate_database();
   generate_block();
   validate_database();

   {
      const account_object& init_acct = db.get_account( MUSE_INIT_MINER_NAME );
      db.modify( init_acct, [&]( account_object& acct ) {
          acct.active.add_authority( init_account_pub_key, acct.active.weight_threshold );
      });
      const witness_object& init_witness = db.get_witness( MUSE_INIT_MINER_NAME );
      db.modify( init_witness, [&]( witness_object& witness ) {
         witness.signing_key = init_account_pub_key;
      });
   }

   if( num_hardforks > 0 )
      db.set_hardfork( num_hardforks );
   vest( MUSE_INIT_MINER_NAME, 10000 );

   // Fill up the rest of the required miners
   for( int i = MUSE_NUM_INIT_MINERS; i < MUSE_MAX_MINERS; i++ )
   {
      account_create( MUSE_INIT_MINER_NAME + fc::to_string( i ), init_account_pub_key );
      fund( MUSE_INIT_MINER_NAME + fc::to_string( i ), MUSE_MIN_PRODUCER_REWARD.amount.value );
      witness_create( MUSE_INIT_MINER_NAME + fc::to_string( i ), init_account_priv_key, "foo.bar", init_account_pub_key, MUSE_MIN_PRODUCER_REWARD.amount );
   }

   validate_database();
} FC_LOG_AND_RETHROW() }

clean_database_fixture::~clean_database_fixture()
{ try {
   // If we're unwinding due to an exception, don't do any more checks.
   // This way, boost test's last checkpoint tells us approximately where the error was.
   if( !std::uncaught_exception() )
   {
      BOOST_CHECK( db.get_node_properties().skip_flags == database::skip_nothing );
   }

   if( data_dir )
      db.close();
   return;
} FC_CAPTURE_AND_LOG( ("Exception in clean_database_fixture destructor") ) }

live_database_fixture::live_database_fixture()
{
   try
   {
      ilog( "Loading saved chain" );
      _chain_dir = fc::current_path() / "test_blockchain";
      FC_ASSERT( fc::exists( _chain_dir ), "Requires blockchain to test on in ./test_blockchain" );

      db.open( _chain_dir, genesis_state_type(), "TEST" );

      auto ahplugin = app.register_plugin< muse::account_history::account_history_plugin >();
      ahplugin->plugin_set_app( &app );
      ahplugin->plugin_initialize( boost::program_options::variables_map() );

      validate_database();
      generate_block();

      ilog( "Done loading saved chain" );
   }
   FC_LOG_AND_RETHROW()
}

live_database_fixture::~live_database_fixture()
{
   try
   {
      // If we're unwinding due to an exception, don't do any more checks.
      // This way, boost test's last checkpoint tells us approximately where the error was.
      if( !std::uncaught_exception() )
      {
         BOOST_CHECK( db.get_node_properties().skip_flags == database::skip_nothing );
      }

      db.pop_block();
      db.close();
      return;
   }
   FC_CAPTURE_AND_LOG( ("Exception in clean_database_fixture destructor") )
}

fc::ecc::private_key database_fixture::generate_private_key(string seed)
{
   static const fc::ecc::private_key committee = fc::ecc::private_key::regenerate( fc::sha256::hash( string( "init_key" ) ) );
   if( seed == "init_key" )
      return committee;
   return fc::ecc::private_key::regenerate( fc::sha256::hash( seed ) );
}

string database_fixture::generate_anon_acct_name()
{
   // names of the form "anon-acct-x123" ; the "x" is necessary
   //    to workaround issue #46
   return "anon-acct-x" + std::to_string( anon_acct_count++ );
}

static genesis_state_type prepare_genesis() {
   genesis_state_type result;
   result.init_supply = 10000 * asset::scaled_precision( MUSE_ASSET_PRECISION );
   {
      const fc::ecc::private_key balance1 = fc::ecc::private_key::regenerate( fc::sha256::hash( string( "balance_key_1" ) ) );
      genesis_state_type::initial_balance_type balance;
      balance.owner = balance1.get_public_key();
      balance.asset_symbol = "2.28.0";
      balance.amount = 1;
      result.initial_balances.push_back( balance );
   }
   return result;
}

void database_fixture::open_database()
{
   if( !data_dir ) {
      data_dir = fc::temp_directory( graphene::utilities::temp_directory_path() );
      const genesis_state_type genesis = prepare_genesis();
      db.open( data_dir->path(), genesis, "test" );
   }
}

signed_block database_fixture::generate_block(uint32_t skip, const fc::ecc::private_key& key, int miss_blocks)
{
   auto witness = db.get_scheduled_witness(miss_blocks + 1);
   auto time = db.get_slot_time(miss_blocks + 1);
   skip |= database::skip_undo_history_check | database::skip_authority_check | database::skip_witness_signature ;
   auto block = db.generate_block(time, witness, key, skip);
   db.clear_pending();
   return block;
}

void database_fixture::generate_blocks( uint32_t block_count )
{
   for( uint32_t i = 0; i < block_count; ++i )
      generate_block();
}

void database_fixture::generate_blocks(fc::time_point_sec timestamp, bool miss_intermediate_blocks)
{
   if( miss_intermediate_blocks )
   {
      generate_block();
      auto slots_to_miss = db.get_slot_at_time(timestamp);
      if( slots_to_miss <= 1 )
         return;
      --slots_to_miss;
      generate_block(0, init_account_priv_key, slots_to_miss);
      return;
   }
   while( db.head_block_time() < timestamp )
      generate_block();

   BOOST_REQUIRE( db.head_block_time() == timestamp );
}

const account_object& database_fixture::account_create(
   const string& name,
   const string& creator,
   const private_key_type& creator_key,
   const share_type& fee,
   const public_key_type& key,
   const public_key_type& post_key,
   const string& json_metadata
   )
{
   try
   {
      account_create_operation op;
      op.new_account_name = name;
      op.creator = creator;
      op.fee = fee;
      op.owner = authority( 1, key, 1 );
      op.active = authority( 1, key, 1 );
      op.basic = authority( 1, post_key, 1 );
      op.memo_key = key;
      op.json_metadata = json_metadata;

      trx.operations.push_back( op );
      trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      trx.sign( creator_key, db.get_chain_id() );
      trx.validate();
      db.push_transaction( trx, 0 );
      trx.operations.clear();
      trx.signatures.clear();

      const account_object& acct = db.get_account( name );

      return acct;
   }
   FC_CAPTURE_AND_RETHROW( (name)(creator) )
}

const account_object& database_fixture::account_create(
   const string& name,
   const public_key_type& key,
   const public_key_type& post_key
)
{
   try
   {
      return account_create(
         name,
         MUSE_INIT_MINER_NAME,
         init_account_priv_key,
         100,
         key,
         post_key,
         "" );
   }
   FC_CAPTURE_AND_RETHROW( (name) );
}

const account_object& database_fixture::account_create(
   const string& name,
   const public_key_type& key
)
{
   return account_create( name, key, key );
}

const witness_object& database_fixture::witness_create(
   const string& owner,
   const private_key_type& owner_key,
   const string& url,
   const public_key_type& signing_key,
   const share_type& fee )
{
   try
   {
      witness_update_operation op;
      op.owner = owner;
      op.url = url;
      op.block_signing_key = signing_key;
      op.fee = asset( fee, MUSE_SYMBOL );

      trx.operations.push_back( op );
      trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      trx.sign( owner_key, db.get_chain_id() );
      trx.validate();
      db.push_transaction( trx, 0 );
      trx.operations.clear();
      trx.signatures.clear();

      return db.get_witness( owner );
   }
   FC_CAPTURE_AND_RETHROW( (owner)(url) )
}

void database_fixture::fund(
   const string& account_name,
   const share_type& amount
   )
{
   try
   {
      transfer( MUSE_INIT_MINER_NAME, account_name, amount );

   } FC_CAPTURE_AND_RETHROW( (account_name)(amount) )
}

void database_fixture::convert(
   const string& account_name,
   const asset& amount )
{
   try
   {
      const account_object& account = db.get_account( account_name );


      if ( amount.asset_id == MUSE_SYMBOL )
      {
         db.adjust_balance( account, -amount );
         db.adjust_balance( account, db.to_mbd( amount ) );
         db.adjust_supply( -amount );
         db.adjust_supply( db.to_mbd( amount ) );
      }
      else if ( amount.asset_id == MBD_SYMBOL )
      {
         db.adjust_balance( account, -amount );
         db.adjust_balance( account, db.to_muse(amount) );
         db.adjust_supply( -amount );
         db.adjust_supply(db.to_muse(amount) );
      }
   } FC_CAPTURE_AND_RETHROW( (account_name)(amount) )
}

void database_fixture::transfer(
   const string& from,
   const string& to,
   const share_type& amount )
{
   try
   {
      transfer_operation op;
      op.from = from;
      op.to = to;
      op.amount = amount;

      trx.operations.push_back( op );
      trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      trx.validate();
      db.push_transaction( trx, ~0 );
      trx.operations.clear();
   } FC_CAPTURE_AND_RETHROW( (from)(to)(amount) )
}

void database_fixture::vest( const string& from, const share_type& amount )
{
   try
   {
      transfer_to_vesting_operation op;
      op.from = from;
      op.to = "";
      op.amount = asset( amount, MUSE_SYMBOL );

      trx.operations.push_back( op );
      trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      trx.validate();
      db.push_transaction( trx, ~0 );
      trx.operations.clear();
   } FC_CAPTURE_AND_RETHROW( (from)(amount) )
}

void database_fixture::proxy( const string& account, const string& proxy )
{
   try
   {
      account_witness_proxy_operation op;
      op.account = account;
      op.proxy = proxy;
      trx.operations.push_back( op );
      db.push_transaction( trx, ~0 );
      trx.operations.clear();
   } FC_CAPTURE_AND_RETHROW( (account)(proxy) )
}

void database_fixture::set_price_feed( const price& new_price )
{
   try
   {
      for ( int i = 1; i < 8; i++ )
      {
         feed_publish_operation op;
         op.publisher = MUSE_INIT_MINER_NAME + fc::to_string( i );
         op.exchange_rate = new_price;
         trx.operations.push_back( op );
         trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
         db.push_transaction( trx, ~0 );
         trx.operations.clear();
      }
   } FC_CAPTURE_AND_RETHROW( (new_price) )

   generate_blocks( MUSE_BLOCKS_PER_HOUR );
   BOOST_REQUIRE( feed_history_id_type()( db ).actual_median_history == new_price );
   BOOST_REQUIRE( feed_history_id_type()( db ).effective_median_history == new_price );
}

const asset& database_fixture::get_balance( const string& account_name )const
{
  return db.get_account( account_name ).balance;
}

void database_fixture::sign(signed_transaction& trx, const fc::ecc::private_key& key)
{
   trx.sign( key, db.get_chain_id() );
}

vector< operation > database_fixture::get_last_operations( uint32_t num_ops )
{
   vector< operation > ops;
   const auto& acc_hist_idx = db.get_index_type< account_history_index >().indices().get< by_id >();
   auto itr = acc_hist_idx.end();

   while( itr != acc_hist_idx.begin() && ops.size() < num_ops )
   {
      itr--;
      ops.push_back( itr->op(db).op );
   }

   return ops;
}

void database_fixture::validate_database( void )
{
   try
   {
      const auto& account_idx = db.get_index_type< account_index >().indices().get< by_id >();
      asset total_supply = asset( 0, MUSE_SYMBOL );
      asset total_mbd = asset( 0, MBD_SYMBOL );
      asset total_vesting = asset( 0, VESTS_SYMBOL );
      share_type total_vsf_votes = share_type( 0 );

      for( auto itr = account_idx.begin(); itr != account_idx.end(); itr++ )
      {
         total_supply += itr->balance;
         total_mbd += itr->mbd_balance;
         total_vesting += itr->vesting_shares;
         total_vsf_votes += ( itr->proxy == MUSE_PROXY_TO_SELF_ACCOUNT ?
                                 itr->witness_vote_weight() :
                                 ( MUSE_MAX_PROXY_RECURSION_DEPTH > 0 ?
                                      itr->proxied_vsf_votes[MUSE_MAX_PROXY_RECURSION_DEPTH - 1] :
                                      itr->vesting_shares.amount ) );
      }

      const auto& convert_request_idx = db.get_index_type< convert_index >().indices().get< by_id >();

      for( auto itr = convert_request_idx.begin(); itr != convert_request_idx.end(); itr++ )
      {
         if( itr->amount.asset_id == MUSE_SYMBOL )
            total_supply += itr->amount;
         else if( itr->amount.asset_id == MBD_SYMBOL )
            total_mbd += itr->amount;
         else
            BOOST_CHECK( !"Encountered illegal symbol in convert_request_object" );
      }

      const auto& limit_order_idx = db.get_index_type< limit_order_index >().indices().get< by_id >();

      for( auto itr = limit_order_idx.begin(); itr != limit_order_idx.end(); itr++ )
      {
         if( itr->sell_price.base.asset_id == MUSE_SYMBOL )
         {
            total_supply += asset( itr->for_sale, MUSE_SYMBOL );
         }
         else if ( itr->sell_price.base.asset_id == MBD_SYMBOL )
         {
            total_mbd += asset( itr->for_sale, MBD_SYMBOL );
         }
      }

      const auto& balance_idx = db.get_index_type< balance_index >().indices().get< by_id >();
      for( const auto& balance : balance_idx )
      {
         if( balance.balance.asset_id == MUSE_SYMBOL )
            total_supply += balance.balance;
         else if ( balance.balance.asset_id == MBD_SYMBOL )
            total_mbd += balance.balance;
         else
            BOOST_CHECK( !"Encountered illegal symbol in initial balance" );
      }

      auto gpo = db.get_dynamic_global_properties();

      if( db.has_hardfork( MUSE_HARDFORK_0_2 ) )
      {
         const auto& content_idx = db.get_index_type< content_index >().indices().get< by_id >();
         for( auto itr = content_idx.begin(); itr != content_idx.end(); itr++ )
         {
            total_supply += itr->accumulated_balance_master;
            total_supply += itr->accumulated_balance_comp;
         }
      }
      else
         total_supply += gpo.total_reward_fund_muse;

      total_supply += gpo.total_vesting_fund_muse;

      FC_ASSERT( gpo.current_supply == total_supply, "", ("gpo.current_supply",gpo.current_supply)("total_supply",total_supply) );
      FC_ASSERT( gpo.current_mbd_supply == total_mbd, "", ("gpo.current_mbd_supply",gpo.current_mbd_supply)("total_mbd",total_mbd) );
      FC_ASSERT( gpo.total_vesting_shares == total_vesting, "", ("gpo.total_vesting_shares",gpo.total_vesting_shares)("total_vesting",total_vesting) );
      FC_ASSERT( gpo.total_vesting_shares.amount == total_vsf_votes, "", ("total_vesting_shares",gpo.total_vesting_shares)("total_vsf_votes",total_vsf_votes) );
      if ( !db.get_feed_history().effective_median_history.is_null() )
         BOOST_REQUIRE( gpo.current_mbd_supply * db.get_feed_history().effective_median_history + gpo.current_supply
            == gpo.virtual_supply );

      for( auto itr = account_idx.begin(); itr != account_idx.end(); itr++ )
      {
          uint64_t pre_score = itr->score;
          db.recalculate_score( *itr );
          BOOST_CHECK_EQUAL( pre_score, itr->score );
      }
   }
   FC_LOG_AND_RETHROW();
}

namespace test {

bool _push_block( database& db, const signed_block& b, uint32_t skip_flags /* = 0 */ )
{
   return db.push_block( b, skip_flags);
}

void _push_transaction( database& db, const signed_transaction& tx, uint32_t skip_flags /* = 0 */ )
{ try {
   db.push_transaction( tx, skip_flags );
} FC_CAPTURE_AND_RETHROW((tx)) }

} // muse::chain::test

} } // muse::chain
