#include <boost/test/unit_test.hpp>

#include <muse/chain/database.hpp>
#include <muse/chain/exceptions.hpp>
#include <muse/chain/hardfork.hpp>

#include <muse/chain/base_objects.hpp>

#include <fc/crypto/digest.hpp>

#include "../common/database_fixture.hpp"

#include <cmath>
#include <iostream>

using namespace muse::chain;
using namespace muse::chain::test;

BOOST_FIXTURE_TEST_SUITE( operation_tests, clean_database_fixture )

BOOST_AUTO_TEST_CASE( account_create_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_create_authorities" );

      signed_transaction tx;
      ACTORS( (alice) );

      private_key_type priv_key = generate_private_key( "temp_key" );

      account_create_operation op;
      op.fee = asset( 10, MUSE_SYMBOL );
      op.new_account_name = "bob";
      op.creator = MUSE_INIT_MINER_NAME;
      op.owner = authority( 1, priv_key.get_public_key(), 1 );
      op.active = authority( 2, priv_key.get_public_key(), 2 );
      op.memo_key = priv_key.get_public_key();
      op.json_metadata = "{\"foo\":\"bar\"}";

      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "--- Test failure when no signatures" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test success with witness signature" );
      tx.sign( init_account_priv_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_TEST_MESSAGE( "--- Test failure when duplicate signatures" );
      tx.operations.clear();
      tx.signatures.clear();
      op.new_account_name = "sam";
      tx.operations.push_back( op );
      tx.sign( init_account_priv_key, db.get_chain_id() );
      tx.sign( init_account_priv_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure when signed by an additional signature not in the creator's authority" );
      tx.signatures.clear();
      tx.sign( init_account_priv_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test failure when signed by a signature not in the creator's authority" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_create_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_create_apply" );

      signed_transaction tx;
      private_key_type priv_key = generate_private_key( "alice" );

      const account_object& init = db.get_account( MUSE_INIT_MINER_NAME );
      asset init_starting_balance = init.balance;

      const auto& gpo = db.get_dynamic_global_properties();

      account_create_operation op;

      op.fee = asset( 100000, MUSE_SYMBOL );
      op.new_account_name = "alice";
      op.creator = MUSE_INIT_MINER_NAME;
      op.owner = authority( 1, priv_key.get_public_key(), 1 );
      op.active = authority( 2, priv_key.get_public_key(), 2 );
      op.memo_key = priv_key.get_public_key();
      op.json_metadata = "{\"foo\":\"bar\"}";

      BOOST_TEST_MESSAGE( "--- Test normal account creation" );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );
      tx.sign( init_account_priv_key, db.get_chain_id() );
      tx.validate();
      db.push_transaction( tx, 0 );

      const account_object& acct = db.get_account( "alice" );

      auto vest_shares = gpo.total_vesting_shares;
      auto vests = gpo.total_vesting_fund_muse;

      BOOST_REQUIRE_EQUAL( acct.name, "alice" );
      BOOST_REQUIRE( acct.owner == authority( 1, priv_key.get_public_key(), 1 ) );
      BOOST_REQUIRE( acct.active == authority( 2, priv_key.get_public_key(), 2 ) );
      BOOST_REQUIRE( acct.memo_key == priv_key.get_public_key() );
      BOOST_REQUIRE_EQUAL( acct.proxy, "" );
      BOOST_REQUIRE( acct.created == db.head_block_time() );
      BOOST_REQUIRE_EQUAL( acct.balance.amount.value, ASSET( "0.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( acct.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );

      #ifndef IS_LOW_MEM
         BOOST_REQUIRE_EQUAL( acct.json_metadata, op.json_metadata );
      #else
         BOOST_REQUIRE_EQUAL( acct.json_metadata, "" );
      #endif

      /// because init_witness has created vesting shares and blocks have been produced, 100 MUSE is worth less than 100 vesting shares due to rounding
      BOOST_REQUIRE_EQUAL( acct.vesting_shares.amount.value, ( op.fee * ( vest_shares / vests ) ).amount.value );
      BOOST_REQUIRE_EQUAL( acct.vesting_withdraw_rate.amount.value, ASSET( "0.000000 2.28.1" ).amount.value );
      BOOST_REQUIRE_EQUAL( acct.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( ( init_starting_balance - ASSET( "0.100000 2.28.0" ) ).amount.value, init.balance.amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure of duplicate account creation" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), fc::assert_exception );

      BOOST_REQUIRE_EQUAL( acct.name, "alice" );
      BOOST_REQUIRE( acct.owner == authority( 1, priv_key.get_public_key(), 1 ) );
      BOOST_REQUIRE( acct.active == authority( 2, priv_key.get_public_key(), 2 ) );
      BOOST_REQUIRE( acct.memo_key == priv_key.get_public_key() );
      BOOST_REQUIRE_EQUAL( acct.proxy, "" );
      BOOST_REQUIRE( acct.created == db.head_block_time() );
      BOOST_REQUIRE_EQUAL( acct.balance.amount.value, ASSET( "0.000 2.28.0 " ).amount.value );
      BOOST_REQUIRE_EQUAL( acct.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( acct.vesting_shares.amount.value, ( op.fee * ( vest_shares / vests ) ).amount.value );
      BOOST_REQUIRE_EQUAL( acct.vesting_withdraw_rate.amount.value, ASSET( "0.000000 2.28.1" ).amount.value );
      BOOST_REQUIRE_EQUAL( acct.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( ( init_starting_balance - ASSET( "0.100000 2.28.0" ) ).amount.value, init.balance.amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when creator cannot cover fee" );
      tx.signatures.clear();
      tx.operations.clear();
      op.fee = asset( db.get_account( MUSE_INIT_MINER_NAME ).balance.amount + 1, MUSE_SYMBOL );
      op.new_account_name = "bob";
      tx.operations.push_back( op );
      tx.sign( init_account_priv_key, db.get_chain_id() );

      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_create_with_delegation_validate )
{ try {
   const private_key_type priv_key = generate_private_key( "temp_key" );

   account_create_with_delegation_operation op;
   op.fee = asset( 10, MUSE_SYMBOL );
   op.delegation = asset( 100, VESTS_SYMBOL );
   op.creator = "alice";
   op.new_account_name = "bob";
   op.owner = authority( 1, priv_key.get_public_key(), 1 );
   op.active = authority( 2, priv_key.get_public_key(), 2 );
   op.memo_key = priv_key.get_public_key();
   op.json_metadata = "{\"foo\":\"bar\"}";
   op.validate();

   // invalid creator
   op.creator = "!alice";
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.creator = "alice";

   // invalid account name
   op.new_account_name = "!alice";
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.new_account_name = "bob";

   // invalid fee
   op.fee = asset( 10, VESTS_SYMBOL );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.fee = asset( -10, MUSE_SYMBOL );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.fee = asset( 10, MUSE_SYMBOL );

   // invalid delegation
   op.delegation = asset( 100, MUSE_SYMBOL );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.delegation = asset( -100, VESTS_SYMBOL );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.delegation = asset( 100, VESTS_SYMBOL );

   // invalid JSON
   op.json_metadata = "{]}";
   BOOST_CHECK_THROW( op.validate(), fc::parse_error_exception );
   op.json_metadata = "{\"foo\":\"bar\"}";

   // invalid owner
   op.owner = authority( 1, "!alice", 1 );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.owner = authority( 1, "alice", 1 );

   // invalid active
   op.active = authority( 1, "!alice", 1 );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.active = authority( 1, "alice", 1 );

   // invalid basic
   op.basic = authority( 1, "!alice", 1 );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.basic = authority( 1, "alice", 1 );

   op.validate();
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( account_create_with_delegation_authorities )
{ try {
   BOOST_TEST_MESSAGE( "Testing: account_create_with_delegation_authorities" );

   account_create_with_delegation_operation op;
   op.creator = "alice";

   flat_set< string > auths;
   flat_set< string > expected;

   op.get_required_owner_authorities( auths );
   BOOST_REQUIRE( auths == expected );

   expected.insert( "alice" );
   op.get_required_active_authorities( auths );
   BOOST_REQUIRE( auths == expected );

   expected.clear();
   auths.clear();
   op.get_required_basic_authorities( auths );
   BOOST_REQUIRE( auths == expected );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( account_create_with_delegation_apply )
{ try {
   BOOST_TEST_MESSAGE( "Testing: account_create_with_delegation_apply" );
   signed_transaction tx;
   ACTORS( (alice) );
   generate_blocks(1);
   fund( "alice", 1500 );
   vest( "alice", 1000 );

   private_key_type priv_key = generate_private_key( "temp_key" );

   generate_block();

   db.modify( db.get_witness_schedule_object(), []( witness_schedule_object& w ) {
      w.median_props.account_creation_fee = asset( 300, MUSE_SYMBOL );
      // actually required VESTS for new account:
      // account_creation_fee * MUSE_CREATE_ACCOUNT_DELEGATION_RATIO * vesting_share_price
      // where fee in MUSE counts as fee * MUSE_CREATE_ACCOUNT_DELEGATION_RATIO * vesting_share_price
      // and delegated shares count as themselves
   });

   generate_block();

   account_create_with_delegation_operation op;
   op.fee = asset( 10, MUSE_SYMBOL );
   op.delegation = asset( 100, VESTS_SYMBOL );
   op.creator = "alice";
   op.new_account_name = "bob";
   op.owner = authority( 1, priv_key.get_public_key(), 1 );
   op.active = authority( 2, priv_key.get_public_key(), 2 );
   op.memo_key = priv_key.get_public_key();
   op.json_metadata = "{\"foo\":\"bar\"}";

   // Alice doesn't have enough VESTS
   op.delegation = asset( 100000000, VESTS_SYMBOL );
   tx.operations.push_back( op );
   tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
   tx.sign( alice_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
   tx.clear();

   // Alice doesn't have enough MUSE
   op.delegation = asset( 100, VESTS_SYMBOL );
   op.fee = asset( 1000, MUSE_SYMBOL );
   tx.operations.push_back( op );
   tx.sign( alice_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
   tx.clear();

   // Insufficient fee
   op.fee = asset( 10, MUSE_SYMBOL );
   tx.operations.push_back( op );
   tx.sign( alice_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
   tx.clear();

   // Required: 300*5*1000 = 1500000
   op.fee = asset( 100, MUSE_SYMBOL );
   op.delegation = asset( 1000000, VESTS_SYMBOL );
   // Present: 100 MUSE*5*1000 + 1000000 VESTS
   tx.operations.push_back( op );
   tx.sign( alice_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );
   tx.clear();

   const account_object& alice_new = db.get_account("alice");
   const account_object& bob = db.get_account("bob");
   BOOST_CHECK_EQUAL( 1000000, bob.received_vesting_shares.amount.value );
   BOOST_CHECK_EQUAL( 100000, bob.vesting_shares.amount.value );
   BOOST_CHECK_EQUAL( 1000000, alice_new.delegated_vesting_shares.amount.value );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( account_update_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_update_validate" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_update_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_update_authorities" );

      ACTORS( (alice)(bob) )
      private_key_type active_key = generate_private_key( "new_key" );
      const account_object& acct = db.get_account( "alice" );

      db.modify( acct, [&]( account_object& a )
      {
         a.active = authority( 1, active_key.get_public_key(), 1 );
      });

      account_update_operation op;
      op.account = "alice";
      op.json_metadata = "{\"success\":true}";

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      BOOST_TEST_MESSAGE( "  Tests when owner authority is not updated ---" );
      BOOST_TEST_MESSAGE( "--- Test failure when no signature" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when wrong signature" );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

/*      BOOST_TEST_MESSAGE( "--- Test failure when containing additional incorrect signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test failure when containing duplicate signatures" );
      tx.signatures.clear();
      tx.sign( active_key, db.get_chain_id() );
      tx.sign( active_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

      BOOST_TEST_MESSAGE( "--- Test success on active key" );
      tx.signatures.clear();
      tx.sign( active_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_TEST_MESSAGE( "--- Test success on owner key alone" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, database::skip_transaction_dupe_check );

      BOOST_TEST_MESSAGE( "  Tests when owner authority is updated ---" );
      BOOST_TEST_MESSAGE( "--- Test failure when updating the owner authority with an active key" );
      tx.signatures.clear();
      tx.operations.clear();
      op.owner = authority( 1, active_key.get_public_key(), 1 );
      tx.operations.push_back( op );
      tx.sign( active_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_owner_auth );

/*      BOOST_TEST_MESSAGE( "--- Test failure when owner key and active key are present" );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test failure when incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0), tx_missing_owner_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when duplicate owner keys are present" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0), tx_duplicate_sig );

      BOOST_TEST_MESSAGE( "--- Test success when updating the owner authority with an owner key" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_update_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_update_apply" );

      ACTORS( (alice) )
      private_key_type new_private_key = generate_private_key( "new_key" );

      BOOST_TEST_MESSAGE( "--- Test normal update" );

      account_update_operation op;
      op.account = "alice";
      op.owner = authority( 1, new_private_key.get_public_key(), 1 );
      op.active = authority( 2, new_private_key.get_public_key(), 2 );
      op.memo_key = new_private_key.get_public_key();
      op.json_metadata = "{\"bar\":\"foo\"}";

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      const account_object& acct = db.get_account( "alice" );

      BOOST_REQUIRE_EQUAL( acct.name, "alice" );
      BOOST_REQUIRE( acct.owner == authority( 1, new_private_key.get_public_key(), 1 ) );
      BOOST_REQUIRE( acct.active == authority( 2, new_private_key.get_public_key(), 2 ) );
      BOOST_REQUIRE( acct.memo_key == new_private_key.get_public_key() );

      #ifndef IS_LOW_MEM
         BOOST_REQUIRE_EQUAL( acct.json_metadata, "{\"bar\":\"foo\"}" );
      #else
         BOOST_REQUIRE_EQUAL( acct.json_metadata, "" );
      #endif

      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when updating a non-existent account" );
      tx.operations.clear();
      tx.signatures.clear();
      op.account = "bob";
      tx.operations.push_back( op );
      tx.sign( new_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception )
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( delegate_vesting_shares_validate )
{ try {
   delegate_vesting_shares_operation op;

   op.delegator = "alice";
   op.delegatee = "bob";
   op.vesting_shares = asset( 1, VESTS_SYMBOL );
   op.validate();

   // invalid delegator
   op.delegator = "!alice";
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.delegator = "alice";

   // invalid delegatee
   op.delegatee = "!alice";
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.delegatee = "bob";

   // invalid self-delegation
   op.delegatee = "alice";
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.delegatee = "bob";

   // invalid amount
   op.vesting_shares = asset( 1, MUSE_SYMBOL );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.vesting_shares = asset( -1, VESTS_SYMBOL );
   BOOST_CHECK_THROW( op.validate(), fc::assert_exception );
   op.vesting_shares = asset( 1, VESTS_SYMBOL );

   op.validate();
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( delegate_vesting_shares_authorities )
{ try {
   BOOST_TEST_MESSAGE( "Testing: delegate_vesting_shares_authorities" );
   signed_transaction tx;
   ACTORS( (alice)(bob) )
   fund( "alice", 1000000 );
   vest( "alice", 1000000 );

   delegate_vesting_shares_operation op;
   op.vesting_shares = asset( 1000000, VESTS_SYMBOL );
   op.delegator = "alice";
   op.delegatee = "bob";

   tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
   tx.operations.push_back( op );

   BOOST_TEST_MESSAGE( "--- Test failure when no signatures" );
   MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

   BOOST_TEST_MESSAGE( "--- Test success with signature" );
   tx.sign( alice_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );
   tx.clear();

   BOOST_TEST_MESSAGE( "--- Test failure when duplicate signatures" );
   op.vesting_shares = asset( 1000001, VESTS_SYMBOL );
   tx.operations.push_back( op );
   tx.sign( alice_private_key, db.get_chain_id() );
   tx.sign( alice_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

   BOOST_TEST_MESSAGE( "--- Test failure when signed by an additional signature not in the creator's authority" );
   tx.signatures.clear();
   tx.sign( init_account_priv_key, db.get_chain_id() );
   tx.sign( alice_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );

   BOOST_TEST_MESSAGE( "--- Test failure when signed by a signature not in the creator's authority" );
   tx.signatures.clear();
   tx.sign( init_account_priv_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );
   validate_database();
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( delegate_vesting_shares_apply )
{ try {
   BOOST_TEST_MESSAGE( "Testing: delegate_vesting_shares_apply" );
   signed_transaction tx;
   ACTORS( (alice)(bob) )
   generate_block();

   fund( "alice", 1000000 );
   vest( "alice", 1000000 );

   generate_block();

   db.modify( db.get_witness_schedule_object(), []( witness_schedule_object& w ) {
      w.median_props.account_creation_fee = asset( 1, MUSE_SYMBOL );
   });

   generate_block();

   delegate_vesting_shares_operation op;
   op.vesting_shares = asset( 1000000, VESTS_SYMBOL );
   op.delegator = "alice";
   op.delegatee = "bob";

   tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
   tx.operations.push_back( op );
   tx.sign( alice_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );
   tx.clear();

   generate_blocks( 1 );

   const account_object& alice_acc = db.get_account( "alice" );
   const account_object& bob_acc = db.get_account( "bob" );

   BOOST_REQUIRE( alice_acc.delegated_vesting_shares == asset( 1000000, VESTS_SYMBOL ) );
   BOOST_REQUIRE( bob_acc.received_vesting_shares == asset( 1000000, VESTS_SYMBOL ) );

   BOOST_TEST_MESSAGE( "--- Test that the delegation object is correct. " );
   const auto& vd_idx = db.get_index_type< vesting_delegation_index >().indices().get< by_delegation >();
   auto delegation = vd_idx.find( boost::make_tuple( op.delegator, op.delegatee ) );

   BOOST_REQUIRE( delegation != vd_idx.end() );
   BOOST_REQUIRE( delegation->delegator == op.delegator);
   BOOST_REQUIRE( delegation->vesting_shares == asset( 1000000, VESTS_SYMBOL ) );

   validate_database();

   op.vesting_shares = asset( 2000000, VESTS_SYMBOL );
   tx.operations.push_back( op );
   tx.sign( alice_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );
   tx.clear();

   generate_blocks(1);

   BOOST_REQUIRE( delegation != vd_idx.end() );
   BOOST_REQUIRE( delegation->delegator == op.delegator );
   BOOST_REQUIRE( delegation->vesting_shares == asset( 2000000, VESTS_SYMBOL ) );
   BOOST_REQUIRE( alice_acc.delegated_vesting_shares == asset( 2000000, VESTS_SYMBOL ) );
   BOOST_REQUIRE( bob_acc.received_vesting_shares == asset( 2000000, VESTS_SYMBOL ) );

   BOOST_TEST_MESSAGE( "--- Test that effective vesting shares is accurate and being applied." );

   generate_block();
   ACTORS( (sam)(dave) )
   generate_block();

   fund( "sam", 1000000 );
   vest( "sam", 1000000 );

   generate_block();

   auto sam_vest = db.get_account( "sam" ).vesting_shares;

   BOOST_TEST_MESSAGE( "--- Test failure when delegating 0 VESTS" );
   tx.clear();
   op.vesting_shares = asset( 0, VESTS_SYMBOL );
   op.delegator = "sam";
   op.delegatee = "dave";
   tx.operations.push_back( op );
   tx.sign( sam_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx ), fc::assert_exception );
   tx.clear();

   BOOST_TEST_MESSAGE( "--- Testing failure delegating more vesting shares than account has." );
   op.vesting_shares = asset( sam_vest.amount + 1, VESTS_SYMBOL );
   tx.operations.push_back( op );
   tx.sign( sam_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx ), fc::assert_exception );

   BOOST_TEST_MESSAGE( "--- Test failure delegating vesting shares that are part of a power down" );
   tx.clear();
   sam_vest = asset( sam_vest.amount / 2, VESTS_SYMBOL );
   withdraw_vesting_operation withdraw;
   withdraw.account = "sam";
   withdraw.vesting_shares = sam_vest;
   tx.operations.push_back( withdraw );
   tx.sign( sam_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );
   tx.clear();

   op.vesting_shares = asset( sam_vest.amount + 2, VESTS_SYMBOL );
   tx.operations.push_back( op );
   tx.sign( sam_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx ), fc::assert_exception );
   tx.clear();

   withdraw.vesting_shares = asset( 0, VESTS_SYMBOL );
   tx.operations.push_back( withdraw );
   tx.sign( sam_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );
   tx.clear();

   BOOST_TEST_MESSAGE( "--- Test failure powering down vesting shares that are delegated" );
   sam_vest.amount += 1000;
   op.vesting_shares = sam_vest;
   tx.operations.push_back( op );
   tx.sign( sam_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );
   tx.clear();

   withdraw.vesting_shares = asset( sam_vest.amount, VESTS_SYMBOL );
   tx.operations.push_back( withdraw );
   tx.sign( sam_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction( tx ), fc::assert_exception );
   tx.clear();

   BOOST_TEST_MESSAGE( "--- Remove a delegation and ensure it is returned after 1 week" );
   op.vesting_shares = asset( 0, VESTS_SYMBOL );
   tx.operations.push_back( op );
   tx.sign( sam_private_key, db.get_chain_id() );
   db.push_transaction( tx, 0 );

   const auto& exp_idx = db.get_index_type< vesting_delegation_expiration_index >().indices().get< by_id >();
   auto exp_obj = exp_idx.begin();
   auto end = exp_idx.end();
   const auto& gpo = db.get_dynamic_global_properties();

   BOOST_REQUIRE( gpo.delegation_return_period == MUSE_DELEGATION_RETURN_PERIOD );

   BOOST_REQUIRE( exp_obj != end );
   BOOST_REQUIRE( exp_obj->delegator == "sam" );
   BOOST_REQUIRE( exp_obj->vesting_shares == sam_vest );
   BOOST_REQUIRE( exp_obj->expiration == db.head_block_time() + gpo.delegation_return_period );
   BOOST_REQUIRE( db.get_account( "sam" ).delegated_vesting_shares == sam_vest );
   BOOST_REQUIRE( db.get_account( "dave" ).received_vesting_shares == asset( 0, VESTS_SYMBOL ) );
   delegation = vd_idx.find( boost::make_tuple( op.delegator, op.delegatee ) );
   BOOST_REQUIRE( delegation == vd_idx.end() );

   generate_blocks( exp_obj->expiration + MUSE_BLOCK_INTERVAL );

   exp_obj = exp_idx.begin();

   BOOST_REQUIRE( exp_obj == end );
   BOOST_REQUIRE( db.get_account( "sam" ).delegated_vesting_shares == asset( 0, VESTS_SYMBOL ) );
} FC_LOG_AND_RETHROW() }

/*
BOOST_AUTO_TEST_CASE( vote_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: vote_validate" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( vote_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: vote_authorities" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( vote_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: vote_apply" );

      ACTORS( (alice)(bob)(sam)(dave) )
      generate_blocks( 60 / MUSE_BLOCK_INTERVAL );

      const auto& vote_idx = db.get_index_type< comment_vote_index >().indices().get< by_comment_voter >();

      {
         const auto& alice = db.get_account( "alice" );
         const auto& bob = db.get_account( "bob" );

         signed_transaction tx;
         comment_operation comment_op;
         comment_op.author = "alice";
         comment_op.permlink = "foo";
         comment_op.parent_permlink = "test";
         comment_op.title = "bar";
         comment_op.body = "foo bar";
         tx.operations.push_back( comment_op );
         tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
         tx.sign( alice_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );

         BOOST_TEST_MESSAGE( "--- Testing voting on a non-existent comment" );

         tx.operations.clear();
         tx.signatures.clear();

         vote_operation op;
         op.voter = "alice";
         op.author = "bob";
         op.permlink = "foo";
         op.weight = MUSE_100_PERCENT;
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );

         MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

         validate_database();

         BOOST_TEST_MESSAGE( "--- Testing voting with a weight of 0" );

         op.weight = (int16_t) 0;
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );

         MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

         validate_database();

         BOOST_TEST_MESSAGE( "--- Testing success" );

         auto old_voting_power = alice.voting_power;

         op.weight = MUSE_100_PERCENT;
         op.author = "alice";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );

         db.push_transaction( tx, 0 );

         auto& alice_comment = db.get_comment( "alice", "foo" );
         auto itr = vote_idx.find( std::make_tuple( alice_comment.id, alice.id ) );

         BOOST_REQUIRE_EQUAL( alice.voting_power, old_voting_power - ( old_voting_power / 200 + 1 ) );
         BOOST_REQUIRE( alice.last_vote_time == db.head_block_time() );
         BOOST_REQUIRE_EQUAL( alice_comment.net_rshares.value, alice.vesting_shares.amount.value * ( old_voting_power - alice.voting_power ) / MUSE_100_PERCENT );
         BOOST_REQUIRE( alice_comment.cashout_time == db.head_block_time() + fc::seconds( MUSE_CASHOUT_WINDOW_SECONDS ) );
         BOOST_REQUIRE( itr->rshares == alice.vesting_shares.amount.value * ( old_voting_power - alice.voting_power ) / MUSE_100_PERCENT );
         BOOST_REQUIRE( itr != vote_idx.end() );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test reduced power for quick voting" );

         old_voting_power = alice.voting_power;

         comment_op.author = "bob";
         comment_op.permlink = "foo";
         comment_op.title = "bar";
         comment_op.body = "foo bar";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( comment_op );
         tx.sign( bob_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );

         op.weight = MUSE_100_PERCENT / 2;
         op.voter = "alice";
         op.author = "bob";
         op.permlink = "foo";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );

         const auto& bob_comment = db.get_comment( "bob", "foo" );
         itr = vote_idx.find( std::make_tuple( bob_comment.id, alice.id ) );

         BOOST_REQUIRE_EQUAL( alice.voting_power, old_voting_power - ( old_voting_power * MUSE_100_PERCENT / ( 400 * MUSE_100_PERCENT ) + 1 ) );
         BOOST_REQUIRE_EQUAL( bob_comment.net_rshares.value, alice.vesting_shares.amount.value * ( old_voting_power - alice.voting_power ) / MUSE_100_PERCENT );
         BOOST_REQUIRE( bob_comment.cashout_time == db.head_block_time() + fc::seconds( MUSE_CASHOUT_WINDOW_SECONDS ) );
         BOOST_REQUIRE( itr != vote_idx.end() );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test payout time extension on vote" );

         uint128_t old_cashout_time = alice_comment.cashout_time.sec_since_epoch();
         old_voting_power = bob.voting_power;
         auto old_abs_rshares = alice_comment.abs_rshares.value;

         generate_blocks( db.head_block_time() + fc::seconds( ( MUSE_CASHOUT_WINDOW_SECONDS / 2 ) ), true );

         const auto& new_bob = db.get_account( "bob" );
         const auto& new_alice_comment = db.get_comment( "alice", "foo" );

         op.weight = MUSE_100_PERCENT;
         op.voter = "bob";
         op.author = "alice";
         op.permlink = "foo";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
         tx.sign( bob_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );

         itr = vote_idx.find( std::make_tuple( new_alice_comment.id, new_bob.id ) );
         uint128_t new_cashout_time = db.head_block_time().sec_since_epoch() + MUSE_CASHOUT_WINDOW_SECONDS;
         const auto& bob_vote_abs_rshares = ( ( uint128_t( new_bob.vesting_shares.amount.value ) * ( ( MUSE_100_PERCENT / 200 ) + 1 ) ) / ( MUSE_100_PERCENT ) ).to_uint64();

         BOOST_REQUIRE_EQUAL( new_bob.voting_power, MUSE_100_PERCENT - ( MUSE_100_PERCENT / 200 + 1 ) );
         BOOST_REQUIRE_EQUAL( new_alice_comment.net_rshares.value, old_abs_rshares + new_bob.vesting_shares.amount.value * ( old_voting_power - new_bob.voting_power ) / MUSE_100_PERCENT );
         BOOST_REQUIRE_EQUAL( new_alice_comment.cashout_time.sec_since_epoch(),
                              ( ( old_cashout_time * old_abs_rshares + new_cashout_time * bob_vote_abs_rshares )
                              / ( old_abs_rshares + bob_vote_abs_rshares ) ).to_uint64() );
         BOOST_REQUIRE( itr != vote_idx.end() );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test negative vote" );

         const auto& new_sam = db.get_account( "sam" );
         const auto& new_bob_comment = db.get_comment( "bob", "foo" );

         old_cashout_time = new_bob_comment.cashout_time.sec_since_epoch();
         old_abs_rshares = new_bob_comment.abs_rshares.value;

         op.weight = -1 * MUSE_100_PERCENT / 2;
         op.voter = "sam";
         op.author = "bob";
         op.permlink = "foo";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( sam_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );

         itr = vote_idx.find( std::make_tuple( new_bob_comment.id, new_sam.id ) );
         new_cashout_time = db.head_block_time().sec_since_epoch() + MUSE_CASHOUT_WINDOW_SECONDS;
         auto sam_weight //= ( ( uint128_t( new_sam.vesting_shares.amount.value ) ) / 400 + 1 ).to_uint64();
                         = ( ( uint128_t( new_sam.vesting_shares.amount.value ) * ( MUSE_100_PERCENT / 400 + 1 ) ) / MUSE_100_PERCENT ).to_uint64();

         BOOST_REQUIRE_EQUAL( new_sam.voting_power, MUSE_100_PERCENT - ( MUSE_100_PERCENT / 400 + 1 ) );
         BOOST_REQUIRE_EQUAL( new_bob_comment.net_rshares.value, old_abs_rshares - sam_weight );
         BOOST_REQUIRE_EQUAL( new_bob_comment.abs_rshares.value, old_abs_rshares + sam_weight );
         BOOST_REQUIRE_EQUAL( new_bob_comment.cashout_time.sec_since_epoch(),
                              ( ( old_cashout_time * old_abs_rshares + new_cashout_time * sam_weight )
                              / ( old_abs_rshares + sam_weight ) ).to_uint64() );
         BOOST_REQUIRE( itr != vote_idx.end() );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test nested voting on nested comments" );

         old_abs_rshares = new_alice_comment.children_abs_rshares.value;
         old_cashout_time = new_alice_comment.cashout_time.sec_since_epoch();
         new_cashout_time = db.head_block_time().sec_since_epoch() + MUSE_CASHOUT_WINDOW_SECONDS;
         int64_t used_power = ( db.get_account( "alice" ).voting_power / 200 ) + 1;

         comment_op.author = "sam";
         comment_op.permlink = "foo";
         comment_op.title = "bar";
         comment_op.body = "foo bar";
         comment_op.parent_author = "alice";
         comment_op.parent_permlink = "foo";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( comment_op );
         tx.sign( sam_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );

         auto old_rshares2 = db.get_comment( "alice", "foo" ).children_rshares2;

         op.weight = MUSE_100_PERCENT;
         op.voter = "alice";
         op.author = "sam";
         op.permlink = "foo";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );

         auto new_rshares = ( ( fc::uint128_t( db.get_account( "alice" ).vesting_shares.amount.value ) * used_power ) / MUSE_100_PERCENT ).to_uint64();

         BOOST_REQUIRE( db.get_comment( "alice", "foo" ).children_rshares2 == db.get_comment( "sam", "foo" ).children_rshares2 + old_rshares2 );
         BOOST_REQUIRE( db.get_comment( "alice", "foo" ).cashout_time.sec_since_epoch() ==
                        ( ( old_cashout_time * old_abs_rshares + new_cashout_time * new_rshares )
                        / ( old_abs_rshares + new_rshares ) ).to_uint64() );

         validate_database();

         BOOST_TEST_MESSAGE( "--- Test increasing vote rshares" );

         auto new_alice = db.get_account( "alice" );
         auto alice_bob_vote = vote_idx.find( std::make_tuple( new_bob_comment.id, new_alice.id ) );
         auto old_vote_rshares = alice_bob_vote->rshares;
         auto old_net_rshares = new_bob_comment.net_rshares.value;
         old_abs_rshares = new_bob_comment.abs_rshares.value;
         old_cashout_time = new_bob_comment.cashout_time.sec_since_epoch();
         new_cashout_time = db.head_block_time().sec_since_epoch() + MUSE_CASHOUT_WINDOW_SECONDS;
         used_power = ( ( ( MUSE_1_PERCENT * 25 * new_alice.voting_power ) / MUSE_100_PERCENT ) / 200 ) + 1;
         auto alice_voting_power = new_alice.voting_power - used_power;

         op.voter = "alice";
         op.weight = MUSE_1_PERCENT * 25;
         op.author = "bob";
         op.permlink = "foo";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );
         alice_bob_vote = vote_idx.find( std::make_tuple( new_bob_comment.id, new_alice.id ) );

         new_rshares = ( ( fc::uint128_t( new_alice.vesting_shares.amount.value ) * used_power ) / MUSE_100_PERCENT ).to_uint64();

         BOOST_REQUIRE( new_bob_comment.net_rshares == old_net_rshares - old_vote_rshares + new_rshares );
         BOOST_REQUIRE( new_bob_comment.abs_rshares == old_abs_rshares + new_rshares );
         BOOST_REQUIRE_EQUAL( new_bob_comment.cashout_time.sec_since_epoch(),
                              ( ( old_cashout_time * old_abs_rshares + new_cashout_time * new_rshares )
                              / ( old_abs_rshares + new_rshares ) ).to_uint64() );
         BOOST_REQUIRE( alice_bob_vote->rshares == new_rshares );
         BOOST_REQUIRE( alice_bob_vote->last_update == db.head_block_time() );
         BOOST_REQUIRE( alice_bob_vote->vote_percent == op.weight );
         BOOST_REQUIRE_EQUAL( db.get_account( "alice" ).voting_power, alice_voting_power );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test decreasing vote rshares" );

         old_vote_rshares = new_rshares;
         old_net_rshares = new_bob_comment.net_rshares.value;
         old_abs_rshares = new_bob_comment.abs_rshares.value;
         old_cashout_time = new_bob_comment.cashout_time.sec_since_epoch();
         used_power = ( uint64_t( MUSE_1_PERCENT ) * 75 * uint64_t( alice_voting_power ) ) / MUSE_100_PERCENT;
         used_power = ( used_power / 200 ) + 1;
         alice_voting_power -= used_power;

         op.weight = MUSE_1_PERCENT * -75;
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );
         alice_bob_vote = vote_idx.find( std::make_tuple( new_bob_comment.id, new_alice.id ) );

         new_rshares = ( ( fc::uint128_t( new_alice.vesting_shares.amount.value ) * used_power ) / MUSE_100_PERCENT ).to_uint64();

         BOOST_REQUIRE( new_bob_comment.net_rshares == old_net_rshares - old_vote_rshares - new_rshares );
         BOOST_REQUIRE( new_bob_comment.abs_rshares == old_abs_rshares + new_rshares );
         BOOST_REQUIRE( new_bob_comment.cashout_time == fc::time_point_sec( ( ( old_cashout_time * old_abs_rshares + new_cashout_time * new_rshares ) / ( old_abs_rshares + new_rshares ) ).to_uint64() ) );
         BOOST_REQUIRE( alice_bob_vote->rshares == -1 * new_rshares );
         BOOST_REQUIRE( alice_bob_vote->last_update == db.head_block_time() );
         BOOST_REQUIRE( alice_bob_vote->vote_percent == op.weight );
         BOOST_REQUIRE( db.get_account( "alice" ).voting_power == alice_voting_power );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test changing a vote to 0 weight (aka: removing a vote)" );

         old_vote_rshares = -1 * new_rshares;
         old_net_rshares = new_bob_comment.net_rshares.value;
         old_abs_rshares = new_bob_comment.abs_rshares.value;
         old_cashout_time = new_bob_comment.cashout_time.sec_since_epoch();
         used_power = 1;
         alice_voting_power -= used_power;

         op.weight = 0;
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );
         alice_bob_vote = vote_idx.find( std::make_tuple( new_bob_comment.id, new_alice.id ) );

         new_rshares = ( ( fc::uint128_t( new_alice.vesting_shares.amount.value ) * used_power ) / MUSE_100_PERCENT ).to_uint64();

         BOOST_REQUIRE( new_bob_comment.net_rshares == old_net_rshares - old_vote_rshares + new_rshares);
         BOOST_REQUIRE( new_bob_comment.abs_rshares == old_abs_rshares + new_rshares);
         BOOST_REQUIRE( new_bob_comment.cashout_time == fc::time_point_sec( ( ( old_cashout_time * old_abs_rshares + new_cashout_time * new_rshares ) / ( old_abs_rshares + new_rshares ) ).to_uint64() ) );
         BOOST_REQUIRE( alice_bob_vote->rshares == new_rshares );
         BOOST_REQUIRE( alice_bob_vote->last_update == db.head_block_time() );
         BOOST_REQUIRE( alice_bob_vote->vote_percent == op.weight );
         BOOST_REQUIRE( db.get_account( "alice" ).voting_power == alice_voting_power );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test failure when increasing rshares within lockout period" );

         generate_blocks( fc::time_point_sec( ( new_bob_comment.cashout_time - MUSE_UPVOTE_LOCKOUT ).sec_since_epoch() + MUSE_BLOCK_INTERVAL ), true );

         op.weight = MUSE_100_PERCENT;
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );
         MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test success when reducing rshares within lockout period" );

         op.weight = -1 * MUSE_100_PERCENT;
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( alice_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );
         validate_database();

         BOOST_TEST_MESSAGE( "--- Test success with a new vote within lockout period" );

         op.weight = MUSE_100_PERCENT;
         op.voter = "sam";
         tx.operations.clear();
         tx.signatures.clear();
         tx.operations.push_back( op );
         tx.sign( sam_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );
         validate_database();
      }
   }
   FC_LOG_AND_RETHROW()
}
*/
BOOST_AUTO_TEST_CASE( transfer_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: transfer_validate" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( transfer_authorities )
{
   try
   {
      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );

      BOOST_TEST_MESSAGE( "Testing: transfer_authorities" );

      transfer_operation op;
      op.from = "alice";
      op.to = "bob";
      op.amount = ASSET( "2.500000 2.28.0" );

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "--- Test failure when no signatures" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when signed by a signature not in the account's authority" );
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when duplicate signatures" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure when signed by an additional signature not in the creator's authority" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test success with witness signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( transfer_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: transfer_apply" );

      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );

      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "10.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET(" 0.000 2.28.0" ).amount.value );

      signed_transaction tx;
      transfer_operation op;

      op.from = "alice";
      op.to = "bob";
      op.amount = ASSET( "5.000 2.28.0" );

      BOOST_TEST_MESSAGE( "--- Test normal transaction" );
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "5.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "5.000 2.28.0" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Generating a block" );
      generate_block();

      const auto& new_alice = db.get_account( "alice" );
      const auto& new_bob = db.get_account( "bob" );

      BOOST_REQUIRE_EQUAL( new_alice.balance.amount.value, ASSET( "5.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( new_bob.balance.amount.value, ASSET( "5.000 2.28.0" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test emptying an account" );
      tx.signatures.clear();
      tx.operations.clear();
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, database::skip_transaction_dupe_check );

      BOOST_REQUIRE_EQUAL( new_alice.balance.amount.value, ASSET( "0.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( new_bob.balance.amount.value, ASSET( "10.000 2.28.0" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test transferring non-existent funds" );
      tx.signatures.clear();
      tx.operations.clear();
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), fc::assert_exception );

      BOOST_REQUIRE_EQUAL( new_alice.balance.amount.value, ASSET( "0.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( new_bob.balance.amount.value, ASSET( "10.000 2.28.0" ).amount.value );
      validate_database();

   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( transfer_to_vesting_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: transfer_to_vesting_validate" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( transfer_to_vesting_authorities )
{
   try
   {
      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );

      BOOST_TEST_MESSAGE( "Testing: transfer_to_vesting_authorities" );

      transfer_to_vesting_operation op;
      op.from = "alice";
      op.to = "bob";
      op.amount = ASSET( "2.500000 2.28.0" );

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "--- Test failure when no signatures" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when signed by a signature not in the account's authority" );
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when duplicate signatures" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure when signed by an additional signature not in the creator's authority" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test success with from signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( transfer_to_vesting_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: transfer_to_vesting_apply" );

      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );

      const auto& gpo = db.get_dynamic_global_properties();

      BOOST_REQUIRE( alice.balance == ASSET( "10.000 2.28.0" ) );

      auto shares = asset( gpo.total_vesting_shares.amount, VESTS_SYMBOL );
      auto vests = asset( gpo.total_vesting_fund_muse.amount, MUSE_SYMBOL );
      auto alice_shares = alice.vesting_shares;
      auto bob_shares = bob.vesting_shares;

      transfer_to_vesting_operation op;
      op.from = "alice";
      op.to = "";
      op.amount = ASSET( "7.500000 2.28.0" );

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      auto new_vest = op.amount * ( shares / vests );
      shares += new_vest;
      vests += op.amount;
      alice_shares += new_vest;

      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "2.500000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.vesting_shares.amount.value, alice_shares.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_vesting_fund_muse.amount.value, vests.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_vesting_shares.amount.value, shares.amount.value );
      validate_database();

      op.to = "bob";
      op.amount = asset( 2000000, MUSE_SYMBOL );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      new_vest = asset( ( op.amount * ( shares / vests ) ).amount, VESTS_SYMBOL );
      shares += new_vest;
      vests += op.amount;
      bob_shares += new_vest;

      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "0.500000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.vesting_shares.amount.value, alice_shares.amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "0.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.vesting_shares.amount.value, bob_shares.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_vesting_fund_muse.amount.value, vests.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_vesting_shares.amount.value, shares.amount.value );
      validate_database();

      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), fc::assert_exception );

      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "0.500000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.vesting_shares.amount.value, alice_shares.amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "0.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.vesting_shares.amount.value, bob_shares.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_vesting_fund_muse.amount.value, vests.amount.value );
      BOOST_REQUIRE_EQUAL( gpo.total_vesting_shares.amount.value, shares.amount.value );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( withdraw_vesting_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: withdraw_vesting_validate" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( withdraw_vesting_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: withdraw_vesting_authorities" );

      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );
      vest( "alice", 10000000 );

      withdraw_vesting_operation op;
      op.account = "alice";
      op.vesting_shares = ASSET( "0.001000 2.28.1" );

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      BOOST_TEST_MESSAGE( "--- Test failure when no signature." );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test success with account signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, database::skip_transaction_dupe_check );

      BOOST_TEST_MESSAGE( "--- Test failure with duplicate signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure with additional incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test failure with incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( withdraw_vesting_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: withdraw_vesting_apply" );

      ACTORS( (alice) )
      fund( "alice", 10000000 );
      vest( "alice", 10000000 );

      BOOST_TEST_MESSAGE( "--- Test failure withdrawing negative VESTS" );

      withdraw_vesting_operation op;
      op.account = "alice";
      op.vesting_shares = asset( -1, VESTS_SYMBOL );

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      {
         proposal_create_operation pop;
         pop.proposed_ops.emplace_back( op );
         pop.expiration_time = db.head_block_time() + fc::minutes(1);
         tx.clear();
         tx.operations.push_back( pop );
         BOOST_CHECK_THROW( PUSH_TX( db, tx ), fc::assert_exception );
      }

      BOOST_TEST_MESSAGE( "--- Test withdraw of existing VESTS" );
      op.vesting_shares = asset( db.get_account("alice").vesting_shares.amount / 2, VESTS_SYMBOL );

      auto old_vesting_shares = db.get_account("alice").vesting_shares;

      tx.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      {
         proposal_create_operation pop;
         pop.proposed_ops.emplace_back( op );
         pop.expiration_time = db.head_block_time() + fc::minutes(1);
         tx.clear();
         tx.operations.push_back( pop );
         PUSH_TX( db, tx );
      }

      BOOST_REQUIRE_EQUAL( db.get_account("alice").vesting_shares.amount.value, old_vesting_shares.amount.value );
      BOOST_REQUIRE_EQUAL( db.get_account("alice").vesting_withdraw_rate.amount.value, ( old_vesting_shares.amount / 2 / MUSE_VESTING_WITHDRAW_INTERVALS ).value );
      BOOST_REQUIRE_EQUAL( db.get_account("alice").to_withdraw.value, op.vesting_shares.amount.value );
      BOOST_REQUIRE( db.get_account("alice").next_vesting_withdrawal == db.head_block_time() + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test changing vesting withdrawal" );
      tx.operations.clear();
      tx.signatures.clear();

      op.vesting_shares = asset( db.get_account("alice").vesting_shares.amount / 3, VESTS_SYMBOL );
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( db.get_account("alice").vesting_shares.amount.value, old_vesting_shares.amount.value );
      BOOST_REQUIRE_EQUAL( db.get_account("alice").vesting_withdraw_rate.amount.value, ( old_vesting_shares.amount / 3 / MUSE_VESTING_WITHDRAW_INTERVALS ).value );
      BOOST_REQUIRE_EQUAL( db.get_account("alice").to_withdraw.value, op.vesting_shares.amount.value );
      BOOST_REQUIRE( db.get_account("alice").next_vesting_withdrawal == db.head_block_time() + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test withdrawing more VESTS than available" );
      auto old_withdraw_amount = db.get_account("alice").to_withdraw;
      tx.operations.clear();
      tx.signatures.clear();

      op.vesting_shares = asset( db.get_account("alice").vesting_shares.amount * 2, VESTS_SYMBOL );
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE_EQUAL( db.get_account("alice").to_withdraw.value, old_withdraw_amount.value );
      BOOST_REQUIRE_EQUAL( db.get_account("alice").vesting_shares.amount.value, old_vesting_shares.amount.value );
      BOOST_REQUIRE_EQUAL( db.get_account("alice").vesting_withdraw_rate.amount.value, ( old_vesting_shares.amount / 3 / MUSE_VESTING_WITHDRAW_INTERVALS ).value );
      BOOST_REQUIRE( db.get_account("alice").next_vesting_withdrawal == db.head_block_time() + MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test withdrawing 0 to resent vesting withdraw" );
      tx.operations.clear();
      tx.signatures.clear();

      op.vesting_shares = asset( 0, VESTS_SYMBOL );
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( db.get_account("alice").vesting_shares.amount.value, old_vesting_shares.amount.value );
      BOOST_REQUIRE_EQUAL( db.get_account("alice").vesting_withdraw_rate.amount.value, 0 );
      BOOST_REQUIRE_EQUAL( db.get_account("alice").to_withdraw.value, 0 );
      BOOST_REQUIRE( db.get_account("alice").next_vesting_withdrawal == fc::time_point_sec::maximum() );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( witness_update_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: withness_update_validate" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( witness_update_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: witness_update_authorities" );

      ACTORS( (alice)(bob) );
      fund( "alice", 10000000 );

      private_key_type signing_key = generate_private_key( "new_key" );

      witness_update_operation op;
      op.owner = "alice";
      op.url = "foo.bar";
      op.fee = ASSET( "1.000 2.28.0" );
      op.block_signing_key = signing_key.get_public_key();

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "--- Test failure when no signatures" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when signed by a signature not in the account's authority" );
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when duplicate signatures" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure when signed by an additional signature not in the creator's authority" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test success with witness signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      tx.signatures.clear();
      tx.sign( signing_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( witness_update_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: witness_update_apply" );

      ACTORS( (alice) )
      fund( "alice", 10000000 );

      private_key_type signing_key = generate_private_key( "new_key" );

      BOOST_TEST_MESSAGE( "--- Test upgrading an account to a witness" );

      witness_update_operation op;
      op.owner = "alice";
      op.url = "foo.bar";
      op.fee = ASSET( "1.000 2.28.0" );
      op.block_signing_key = signing_key.get_public_key();
      op.props.account_creation_fee = asset( MUSE_MIN_ACCOUNT_CREATION_FEE + 10, MUSE_SYMBOL);
      op.props.maximum_block_size = MUSE_MIN_BLOCK_SIZE_LIMIT + 100;

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      const witness_object& alice_witness = db.get_witness( "alice" );

      BOOST_REQUIRE_EQUAL( alice_witness.owner, "alice" );
      BOOST_REQUIRE( alice_witness.created == db.head_block_time() );
      BOOST_REQUIRE_EQUAL( alice_witness.url, op.url );
      BOOST_REQUIRE( alice_witness.signing_key == op.block_signing_key );
      BOOST_REQUIRE( alice_witness.props.account_creation_fee == op.props.account_creation_fee );
      BOOST_REQUIRE_EQUAL( alice_witness.props.maximum_block_size, op.props.maximum_block_size );
      BOOST_REQUIRE_EQUAL( alice_witness.total_missed, 0 );
      BOOST_REQUIRE_EQUAL( alice_witness.last_aslot, 0 );
      BOOST_REQUIRE_EQUAL( alice_witness.last_confirmed_block_num, 0 );
      BOOST_REQUIRE_EQUAL( alice_witness.votes.value, 0 );
      BOOST_REQUIRE( alice_witness.virtual_last_update == 0 );
      BOOST_REQUIRE( alice_witness.virtual_position == 0 );
      BOOST_REQUIRE( alice_witness.virtual_scheduled_time == fc::uint128_t::max_value() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "10.000 2.28.0" ).amount.value ); // No fee
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test updating a witness" );

      tx.signatures.clear();
      tx.operations.clear();
      op.url = "bar.foo";
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( alice_witness.owner, "alice" );
      BOOST_REQUIRE( alice_witness.created == db.head_block_time() );
      BOOST_REQUIRE_EQUAL( alice_witness.url, "bar.foo" );
      BOOST_REQUIRE( alice_witness.signing_key == op.block_signing_key );
      BOOST_REQUIRE( alice_witness.props.account_creation_fee == op.props.account_creation_fee );
      BOOST_REQUIRE_EQUAL( alice_witness.props.maximum_block_size, op.props.maximum_block_size );
      BOOST_REQUIRE_EQUAL( alice_witness.total_missed, 0 );
      BOOST_REQUIRE_EQUAL( alice_witness.last_aslot, 0 );
      BOOST_REQUIRE_EQUAL( alice_witness.last_confirmed_block_num, 0 );
      BOOST_REQUIRE_EQUAL( alice_witness.votes.value, 0 );
      BOOST_REQUIRE( alice_witness.virtual_last_update == 0 );
      BOOST_REQUIRE( alice_witness.virtual_position == 0 );
      BOOST_REQUIRE( alice_witness.virtual_scheduled_time == fc::uint128_t::max_value() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "10.000 2.28.0" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when upgrading a non-existent account" );

      tx.signatures.clear();
      tx.operations.clear();
      op.owner = "bob";
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_witness_vote_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_witness_vote_validate" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_witness_vote_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_witness_vote_authorities" );

      ACTORS( (alice)(bob)(sam) )

      fund( "alice", 1000000 );
      private_key_type alice_witness_key = generate_private_key( "alice_witness" );
      witness_create( "alice", alice_private_key, "foo.bar", alice_witness_key.get_public_key(), 1000 );

      account_witness_vote_operation op;
      op.account = "bob";
      op.witness = "alice";

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "--- Test failure when no signatures" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_basic_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when signed by a signature not in the account's authority" );
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_basic_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when duplicate signatures" );
      tx.signatures.clear();
      tx.sign( bob_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

      BOOST_TEST_MESSAGE( "--- Test failure when signed by an additional signature not in the creator's authority" );
      tx.signatures.clear();
      tx.sign( bob_private_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );

      BOOST_TEST_MESSAGE( "--- Test success with witness signature" );
      tx.signatures.clear();
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_TEST_MESSAGE( "--- Test failure with proxy signature" );
      proxy( "bob", "sam" );
      tx.signatures.clear();
      tx.sign( sam_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_basic_auth );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_witness_vote_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_witness_vote_apply" );

      ACTORS( (alice)(bob)(sam) )
      fund( "alice" , 5000000 );
      vest( "alice", 5000000 );
      fund( "sam", 1000000 );

      private_key_type sam_witness_key = generate_private_key( "sam_key" );
      witness_create( "sam", sam_private_key, "foo.bar", sam_witness_key.get_public_key(), 1000 );
      const witness_object& sam_witness = db.get_witness( "sam" );

      const auto& witness_vote_idx = db.get_index_type< witness_vote_index >().indices().get< by_witness_account >();

      BOOST_TEST_MESSAGE( "--- Test normal vote" );
      account_witness_vote_operation op;
      op.account = "alice";
      op.witness = "sam";
      op.approve = true;

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( sam_witness.votes == alice.vesting_shares.amount );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, alice.id ) ) != witness_vote_idx.end() );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test revoke vote" );
      op.approve = false;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );
      BOOST_REQUIRE_EQUAL( sam_witness.votes.value, 0 );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, alice.id ) ) == witness_vote_idx.end() );

      BOOST_TEST_MESSAGE( "--- Test failure when attempting to revoke a non-existent vote" );

      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), fc::assert_exception );
      BOOST_REQUIRE_EQUAL( sam_witness.votes.value, 0 );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, alice.id ) ) == witness_vote_idx.end() );

      BOOST_TEST_MESSAGE( "--- Test proxied vote" );
      proxy( "alice", "bob" );
      tx.operations.clear();
      tx.signatures.clear();
      op.approve = true;
      op.account = "bob";
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( sam_witness.votes == ( bob.proxied_vsf_votes_total() + bob.vesting_shares.amount ) );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, bob.id ) ) != witness_vote_idx.end() );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, alice.id ) ) == witness_vote_idx.end() );

      BOOST_TEST_MESSAGE( "--- Test vote from a proxied account" );
      tx.operations.clear();
      tx.signatures.clear();
      op.account = "alice";
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), fc::assert_exception );

      BOOST_REQUIRE( sam_witness.votes == ( bob.proxied_vsf_votes_total() + bob.vesting_shares.amount ) );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, bob.id ) ) != witness_vote_idx.end() );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, alice.id ) ) == witness_vote_idx.end() );

      BOOST_TEST_MESSAGE( "--- Test revoke proxied vote" );
      tx.operations.clear();
      tx.signatures.clear();
      op.account = "bob";
      op.approve = false;
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( sam_witness.votes.value, 0 );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, bob.id ) ) == witness_vote_idx.end() );
      BOOST_REQUIRE( witness_vote_idx.find( std::make_tuple( sam_witness.id, alice.id ) ) == witness_vote_idx.end() );

      BOOST_TEST_MESSAGE( "--- Test failure when voting for a non-existent account" );
      tx.operations.clear();
      tx.signatures.clear();
      op.witness = "dave";
      op.approve = true;
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );

      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when voting for an account that is not a witness" );
      tx.operations.clear();
      tx.signatures.clear();
      op.witness = "alice";
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );

      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_witness_proxy_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_witness_proxy_validate" );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_witness_proxy_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_witness_proxy_authorities" );

      ACTORS( (alice)(bob) )

      account_witness_proxy_operation op;
      op.account = "bob";
      op.proxy = "alice";

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "--- Test failure when no signatures" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_basic_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when signed by a signature not in the account's authority" );
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_basic_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when duplicate signatures" );
      tx.signatures.clear();
      tx.sign( bob_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

      BOOST_TEST_MESSAGE( "--- Test failure when signed by an additional signature not in the creator's authority" );
      tx.signatures.clear();
      tx.sign( bob_private_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );

      BOOST_TEST_MESSAGE( "--- Test success with witness signature" );
      tx.signatures.clear();
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_TEST_MESSAGE( "--- Test failure with proxy signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_basic_auth );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_witness_proxy_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account_witness_proxy_apply" );

      ACTORS( (alice)(bob)(sam)(dave) )
      fund( "alice", 1000 );
      vest( "alice", 1000 );
      fund( "bob", 3000 );
      vest( "bob", 3000 );
      fund( "sam", 5000 );
      vest( "sam", 5000 );
      fund( "dave", 7000 );
      vest( "dave", 7000 );

      BOOST_TEST_MESSAGE( "--- Test setting proxy to another account from self." );
      // bob -> alice

      account_witness_proxy_operation op;
      op.account = "bob";
      op.proxy = "alice";

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( bob_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( bob.proxy, "alice" );
      BOOST_REQUIRE_EQUAL( bob.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( alice.proxy, MUSE_PROXY_TO_SELF_ACCOUNT );
      BOOST_REQUIRE( alice.proxied_vsf_votes_total() == bob.vesting_shares.amount );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test changing proxy" );
      // bob->sam

      tx.operations.clear();
      tx.signatures.clear();
      op.proxy = "sam";
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( bob.proxy, "sam" );
      BOOST_REQUIRE_EQUAL( bob.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( alice.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( sam.proxy, MUSE_PROXY_TO_SELF_ACCOUNT );
      BOOST_REQUIRE( sam.proxied_vsf_votes_total().value == bob.vesting_shares.amount );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when changing proxy to existing proxy" );

      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), fc::assert_exception );

      BOOST_REQUIRE_EQUAL( bob.proxy, "sam" );
      BOOST_REQUIRE_EQUAL( bob.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( sam.proxy, MUSE_PROXY_TO_SELF_ACCOUNT );
      BOOST_REQUIRE( sam.proxied_vsf_votes_total() == bob.vesting_shares.amount );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test adding a grandparent proxy" );
      // bob->sam->dave

      tx.operations.clear();
      tx.signatures.clear();
      op.proxy = "dave";
      op.account = "sam";
      tx.operations.push_back( op );
      tx.sign( sam_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( bob.proxy, "sam" );
      BOOST_REQUIRE_EQUAL( bob.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( sam.proxy, "dave" );
      BOOST_REQUIRE( sam.proxied_vsf_votes_total() == bob.vesting_shares.amount );
      BOOST_REQUIRE_EQUAL( dave.proxy, MUSE_PROXY_TO_SELF_ACCOUNT );
      BOOST_REQUIRE( dave.proxied_vsf_votes_total() == ( sam.vesting_shares + bob.vesting_shares ).amount );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test adding a grandchild proxy" );
      // alice->sam->dave

      tx.operations.clear();
      tx.signatures.clear();
      op.proxy = "sam";
      op.account = "alice";
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( alice.proxy, "sam" );
      BOOST_REQUIRE_EQUAL( alice.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( bob.proxy, "sam" );
      BOOST_REQUIRE_EQUAL( bob.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( sam.proxy, "dave" );
      BOOST_REQUIRE( sam.proxied_vsf_votes_total() == ( bob.vesting_shares + alice.vesting_shares ).amount );
      BOOST_REQUIRE_EQUAL( dave.proxy, MUSE_PROXY_TO_SELF_ACCOUNT );
      BOOST_REQUIRE( dave.proxied_vsf_votes_total() == ( sam.vesting_shares + bob.vesting_shares + alice.vesting_shares ).amount );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test removing a grandchild proxy" );
      // alice->sam->dave

      tx.operations.clear();
      tx.signatures.clear();
      op.proxy = MUSE_PROXY_TO_SELF_ACCOUNT;
      op.account = "bob";
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( alice.proxy, "sam" );
      BOOST_REQUIRE_EQUAL( alice.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( bob.proxy, MUSE_PROXY_TO_SELF_ACCOUNT );
      BOOST_REQUIRE_EQUAL( bob.proxied_vsf_votes_total().value, 0 );
      BOOST_REQUIRE_EQUAL( sam.proxy, "dave" );
      BOOST_REQUIRE( sam.proxied_vsf_votes_total() == alice.vesting_shares.amount );
      BOOST_REQUIRE_EQUAL( dave.proxy, MUSE_PROXY_TO_SELF_ACCOUNT );
      BOOST_REQUIRE( dave.proxied_vsf_votes_total() == ( sam.vesting_shares + alice.vesting_shares ).amount );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test votes are transferred when a proxy is added" );
      account_witness_vote_operation vote;
      vote.account= "bob";
      vote.witness = MUSE_INIT_MINER_NAME;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( vote );
      tx.sign( bob_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      tx.operations.clear();
      tx.signatures.clear();
      op.account = "alice";
      op.proxy = "bob";
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( db.get_witness( MUSE_INIT_MINER_NAME ).votes == ( alice.vesting_shares + bob.vesting_shares ).amount );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test votes are removed when a proxy is removed" );
      op.proxy = MUSE_PROXY_TO_SELF_ACCOUNT;
      tx.signatures.clear();
      tx.operations.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( db.get_witness( MUSE_INIT_MINER_NAME ).votes == bob.vesting_shares.amount );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( custom_validate ) {}

BOOST_AUTO_TEST_CASE( custom_authorities ) {}

BOOST_AUTO_TEST_CASE( custom_apply ) {}

BOOST_AUTO_TEST_CASE( feed_publish_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: feed_publish_validate" );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( feed_publish_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: feed_publish_authorities" );

      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );
      witness_create( "alice", alice_private_key, "foo.bar", alice_private_key.get_public_key(), 1000 );

      feed_publish_operation op;
      op.publisher = "alice";
      op.exchange_rate = price( ASSET( "1.000 2.28.0" ), ASSET( "1.000 2.28.2" ) );

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      BOOST_TEST_MESSAGE( "--- Test failure when no signature." );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure with incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure with duplicate signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure with additional incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test success with witness account signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, database::skip_transaction_dupe_check );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( feed_publish_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: feed_publish_apply" );

      ACTORS( (alice) )
      fund( "alice", 10000000 );
      witness_create( "alice", alice_private_key, "foo.bar", alice_private_key.get_public_key(), 1000 );

      BOOST_TEST_MESSAGE( "--- Test publishing price feed" );
      feed_publish_operation op;
      op.publisher = "alice";
      op.exchange_rate = price( ASSET( "1000.000 2.28.0" ), ASSET( "1.000 2.28.2" ) ); // 1000 MUSE : 1 SBD

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      witness_object& alice_witness = const_cast< witness_object& >( db.get_witness( "alice" ) );

      BOOST_REQUIRE( alice_witness.mbd_exchange_rate == op.exchange_rate );
      BOOST_REQUIRE( alice_witness.last_mbd_exchange_update == db.head_block_time() );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure publishing to non-existent witness" );

      tx.operations.clear();
      tx.signatures.clear();
      op.publisher = "bob";
      tx.sign( alice_private_key, db.get_chain_id() );

      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test updating price feed" );

      tx.operations.clear();
      tx.signatures.clear();
      op.exchange_rate = price( ASSET(" 1500.000 2.28.0" ), ASSET( "1.000 2.28.2" ) );
      op.publisher = "alice";
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );

      db.push_transaction( tx, 0 );

      alice_witness = const_cast< witness_object& >( db.get_witness( "alice" ) );
      BOOST_REQUIRE( std::abs( alice_witness.mbd_exchange_rate.to_real() - op.exchange_rate.to_real() ) < 0.0000005 );
      BOOST_REQUIRE( alice_witness.last_mbd_exchange_update == db.head_block_time() );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( convert_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: convert_validate" );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( convert_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: convert_authorities" );

      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );

      set_price_feed( price( ASSET( "1.000 2.28.0" ), ASSET( "1.000 2.28.2" ) ) );

      convert( "alice", ASSET( "2.500000 2.28.0" ) );

      validate_database();

      convert_operation op;
      op.owner = "alice";
      op.amount = ASSET( "2.500000 2.28.2" );

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "--- Test failure when no signatures" );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when signed by a signature not in the account's authority" );
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test failure when duplicate signatures" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure when signed by an additional signature not in the creator's authority" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test success with owner signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( convert_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: convert_apply" );
      ACTORS( (alice)(bob) );
      fund( "alice", 10000000 );
      fund( "bob" , 10000000 );

      convert_operation op;
      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      const auto& convert_request_idx = db.get_index_type< convert_index >().indices().get< by_owner >();

      set_price_feed( price( ASSET( "1.000 2.28.0" ), ASSET( "1.000 2.28.2" ) ) );

      convert( "alice", ASSET( "2.500000 2.28.0" ) );
      convert( "bob", ASSET( "7.000 2.28.0" ) );

      const auto& new_alice = db.get_account( "alice" );
      const auto& new_bob = db.get_account( "bob" );

      BOOST_TEST_MESSAGE( "--- Test failure when account does not have the required MUSE" );
      op.owner = "bob";
      op.amount = ASSET( "5.000 2.28.0" );
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE_EQUAL( new_bob.balance.amount.value, ASSET( "3.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( new_bob.mbd_balance.amount.value, ASSET( "7.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when account does not have the required MBD" );
      op.owner = "alice";
      op.amount = ASSET( "5.000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE_EQUAL( new_alice.balance.amount.value, ASSET( "7.500000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( new_alice.mbd_balance.amount.value, ASSET( "2.500000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when account does not exist" );
      op.owner = "sam";
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_TEST_MESSAGE( "--- Test success converting MBD to MUSE" );
      op.owner = "bob";
      op.amount = ASSET( "3.000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE_EQUAL( new_bob.balance.amount.value, ASSET( "3.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( new_bob.mbd_balance.amount.value, ASSET( "4.000 2.28.2" ).amount.value );

      auto convert_request = convert_request_idx.find( std::make_tuple( op.owner, op.requestid ) );
      BOOST_REQUIRE( convert_request != convert_request_idx.end() );
      BOOST_REQUIRE_EQUAL( convert_request->owner, op.owner );
      BOOST_REQUIRE_EQUAL( convert_request->requestid, op.requestid );
      BOOST_REQUIRE_EQUAL( convert_request->amount.amount.value, op.amount.amount.value );
      //BOOST_REQUIRE_EQUAL( convert_request->premium, 100000 );
      BOOST_REQUIRE( convert_request->conversion_date == db.head_block_time() + MUSE_CONVERSION_DELAY );

      BOOST_TEST_MESSAGE( "--- Test failure from repeated id" );
      op.amount = ASSET( "2.000 2.28.0" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE_EQUAL( new_bob.balance.amount.value, ASSET( "3.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( new_bob.mbd_balance.amount.value, ASSET( "4.000 2.28.2" ).amount.value );

      convert_request = convert_request_idx.find( std::make_tuple( op.owner, op.requestid ) );
      BOOST_REQUIRE( convert_request != convert_request_idx.end() );
      BOOST_REQUIRE_EQUAL( convert_request->owner, op.owner );
      BOOST_REQUIRE_EQUAL( convert_request->requestid, op.requestid );
      BOOST_REQUIRE_EQUAL( convert_request->amount.amount.value, ASSET( "3.000 2.28.2" ).amount.value );
      //BOOST_REQUIRE_EQUAL( convert_request->premium, 100000 );
      BOOST_REQUIRE( convert_request->conversion_date == db.head_block_time() + MUSE_CONVERSION_DELAY );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE( convert_forward, database_fixture )
{ try {

   initialize_clean( 5 );

   ACTORS( (alice)(federation) );
   const account_id_type fed_asset_id = account_create( "federation.asset", federation_public_key ).id;
   fund( "alice", 10000000 );
   fund( "federation", 10000000 );
   fund( "federation.asset", 10000000 );

   // fake a price feed
   generate_block();
   db.modify( db.get_feed_history(), [] ( feed_history_object& fho ){
      fho.effective_median_history = fho.actual_median_history = asset(1) / asset(1, MBD_SYMBOL);
   });
   trx.clear();
   trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

   // convert XSD before hf 0.6 fails
   convert_operation op;
   op.owner = "federation";
   op.amount = asset(1000);
   trx.operations.emplace_back(op);
   BOOST_CHECK_THROW( PUSH_TX( db, trx, database::skip_transaction_signatures ), fc::assert_exception );
   trx.clear();

   // apply hf 0.6
   generate_blocks( 2*MUSE_MAX_MINERS );
   generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_6_TIME + MUSE_BLOCK_INTERVAL ), true );
   trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

   // alice can't convert XSD -> xUSD
   op.owner = "alice";
   trx.operations.emplace_back(op);
   BOOST_CHECK_THROW( PUSH_TX( db, trx, database::skip_transaction_signatures ), fc::assert_exception );
   trx.clear();

   // but federation can
   op.owner = "federation";
   trx.operations.emplace_back(op);
   PUSH_TX( db, trx, database::skip_transaction_signatures );
   trx.clear();
   BOOST_CHECK_EQUAL( federation_id(db).mbd_balance.amount.value, ASSET( "0.001 2.28.2" ).amount.value );

   // and federation.asset can
   op.owner = "federation.asset";
   trx.operations.emplace_back(op);
   PUSH_TX( db, trx, database::skip_transaction_signatures );
   trx.clear();
   BOOST_CHECK_EQUAL( fed_asset_id(db).mbd_balance.amount.value, ASSET( "0.001 2.28.2" ).amount.value );
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( limit_order_create_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: limit_order_create_validate" );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( limit_order_create_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: limit_order_create_authorities" );

      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );

      limit_order_create_operation op;
      op.owner = "alice";
      op.amount_to_sell = ASSET( "1.000 2.28.0" );
      op.min_to_receive = ASSET( "1.000 2.28.2" );

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      BOOST_TEST_MESSAGE( "--- Test failure when no signature." );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test success with account signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, database::skip_transaction_dupe_check );

      BOOST_TEST_MESSAGE( "--- Test failure with duplicate signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure with additional incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test failure with incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( limit_order_create_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: limit_order_create_apply" );

      set_price_feed( price( ASSET( "1.000 2.28.0" ), ASSET( "1.000 2.28.2" ) ) );

      ACTORS( (alice)(bob) )
      fund( "alice", 1000000000 );
      fund( "bob", 1000000000 );
      convert( "bob", ASSET("1000.000 2.28.0" ) );

      const auto& limit_order_idx = db.get_index_type< limit_order_index >().indices().get< by_account >();

      BOOST_TEST_MESSAGE( "--- Test failure when account does not have required funds" );
      limit_order_create_operation op;
      signed_transaction tx;

      op.owner = "bob";
      op.orderid = 1;
      op.amount_to_sell = ASSET( "10.000 2.28.0" );
      op.min_to_receive = ASSET( "10.000 2.28.2" );
      op.fill_or_kill = false;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "bob", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "0.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "1000.0000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when amount to receive is 0" );

      op.owner = "alice";
      op.min_to_receive = ASSET( "0.000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "1000.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when amount to sell is 0" );

      op.amount_to_sell = ASSET( "0.000 2.28.0" );
      op.min_to_receive = ASSET( "10.000 2.28.2" ) ;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "1000.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test success creating limit order that will not be filled" );

      op.amount_to_sell = ASSET( "10.000 2.28.0" );
      op.min_to_receive = ASSET( "15.000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      auto limit_order = limit_order_idx.find( std::make_tuple( "alice", op.orderid ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, op.owner );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, op.orderid );
      BOOST_REQUIRE( limit_order->for_sale == op.amount_to_sell.amount );
      BOOST_REQUIRE( limit_order->sell_price == price( op.amount_to_sell / op.min_to_receive ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure creating limit order with duplicate id" );

      op.amount_to_sell = ASSET( "20.000 2.28.0" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      limit_order = limit_order_idx.find( std::make_tuple( "alice", op.orderid ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, op.owner );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, op.orderid );
      BOOST_REQUIRE( limit_order->for_sale == 10000000 );
      BOOST_REQUIRE( limit_order->sell_price == price( ASSET( "10.000 2.28.0" ), op.min_to_receive ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test sucess killing an order that will not be filled" );

      op.orderid = 2;
      op.fill_or_kill = true;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test having a partial match to limit order" );
      // Alice has order for 15 SBD at a price of 2:3
      // Fill 5 MUSE for 7.5 SBD

      op.owner = "bob";
      op.orderid = 1;
      op.amount_to_sell = ASSET( "7.500000 2.28.2" );
      op.min_to_receive = ASSET( "5.000 2.28.0" );
      op.fill_or_kill = false;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      auto recent_ops = get_last_operations( 1 );
      auto fill_order_op = recent_ops[0].get< fill_order_operation >();

      limit_order = limit_order_idx.find( std::make_tuple( "alice", 1 ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, "alice" );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, op.orderid );
      BOOST_REQUIRE( limit_order->for_sale == 5000000 );
      BOOST_REQUIRE( limit_order->sell_price == price( ASSET( "10.000 2.28.0" ), ASSET( "15.000 2.28.2" ) ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "bob", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "7.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "5.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "992.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( fill_order_op.open_owner, "alice" );
      BOOST_REQUIRE_EQUAL( fill_order_op.open_orderid, 1 );
      BOOST_REQUIRE_EQUAL( fill_order_op.open_pays.amount.value, ASSET( "5.000 2.28.0").amount.value );
      BOOST_REQUIRE_EQUAL( fill_order_op.current_owner, "bob" );
      BOOST_REQUIRE_EQUAL( fill_order_op.current_orderid, 1 );
      BOOST_REQUIRE_EQUAL( fill_order_op.current_pays.amount.value, ASSET( "7.500000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test filling an existing order fully, but the new order partially" );

      op.amount_to_sell = ASSET( "15.000 2.28.2" );
      op.min_to_receive = ASSET( "10.000 2.28.0" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      limit_order = limit_order_idx.find( std::make_tuple( "bob", 1 ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, "bob" );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, 1 );
      BOOST_REQUIRE_EQUAL( limit_order->for_sale.value, 7500000 );
      BOOST_REQUIRE( limit_order->sell_price == price( ASSET( "15.000 2.28.2" ), ASSET( "10.000 2.28.0" ) ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", 1 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "15.000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "10.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "977.500000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test filling an existing order and new order fully" );

      op.owner = "alice";
      op.orderid = 3;
      op.amount_to_sell = ASSET( "5.000 2.28.0" );
      op.min_to_receive = ASSET( "7.500000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", 3 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "bob", 1 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "985.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "22.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "15.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "977.500000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test filling limit order with better order when partial order is better." );

      op.owner = "alice";
      op.orderid = 4;
      op.amount_to_sell = ASSET( "10.000 2.28.0" );
      op.min_to_receive = ASSET( "11.000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      op.owner = "bob";
      op.orderid = 4;
      op.amount_to_sell = ASSET( "12.000 2.28.2" );
      op.min_to_receive = ASSET( "10.000 2.28.0" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      limit_order = limit_order_idx.find( std::make_tuple( "bob", 4 ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE( limit_order_idx.find(std::make_tuple( "alice", 4 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, "bob" );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, 4 );
      BOOST_REQUIRE_EQUAL( limit_order->for_sale.value, 1000000 );
      BOOST_REQUIRE( limit_order->sell_price == price( ASSET( "12.000 2.28.2" ), ASSET( "10.000 2.28.0" ) ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "975.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "33.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "25.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "965.500000 2.28.2" ).amount.value );
      validate_database();

      limit_order_cancel_operation can;
      can.owner = "bob";
      can.orderid = 4;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( can );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_TEST_MESSAGE( "--- Test filling limit order with better order when partial order is worse." );

      auto gpo = db.get_dynamic_global_properties();

      op.owner = "alice";
      op.orderid = 5;
      op.amount_to_sell = ASSET( "20.000 2.28.0" );
      op.min_to_receive = ASSET( "22.000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      op.owner = "bob";
      op.orderid = 5;
      op.amount_to_sell = ASSET( "12.000 2.28.2" );
      op.min_to_receive = ASSET( "10.000 2.28.0" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      limit_order = limit_order_idx.find( std::make_tuple( "alice", 5 ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE( limit_order_idx.find(std::make_tuple( "bob", 5 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, "alice" );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, 5 );
      BOOST_REQUIRE_EQUAL( limit_order->for_sale.value, 9090910 );
      BOOST_REQUIRE( limit_order->sell_price == price( ASSET( "20.000 2.28.0" ), ASSET( "22.000 2.28.2" ) ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "955.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "45.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "35.909090 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "954.500000 2.28.2" ).amount.value );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( limit_order_create2_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: limit_order_create2_authorities" );

      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );

      limit_order_create2_operation op;
      op.owner = "alice";
      op.amount_to_sell = ASSET( "1.000 2.28.0" );
      op.exchange_rate = price( ASSET( "1.000 2.28.0" ), ASSET( "1.000 2.28.2" ) );

      signed_transaction tx;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      BOOST_TEST_MESSAGE( "--- Test failure when no signature." );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test success with account signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, database::skip_transaction_dupe_check );

      BOOST_TEST_MESSAGE( "--- Test failure with duplicate signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_duplicate_sig );

/*      BOOST_TEST_MESSAGE( "--- Test failure with additional incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_private_key, db.get_chain_id() );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_irrelevant_sig );
*/
      BOOST_TEST_MESSAGE( "--- Test failure with incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( limit_order_create2_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: limit_order_create2_apply" );

      set_price_feed( price( ASSET( "1.000 2.28.0" ), ASSET( "1.000 2.28.2" ) ) );

      ACTORS( (alice)(bob) )
      fund( "alice", 1000000000 );
      fund( "bob", 1000000000 );
      convert( "bob", ASSET("1000.000 2.28.0" ) );

      const auto& limit_order_idx = db.get_index_type< limit_order_index >().indices().get< by_account >();

      BOOST_TEST_MESSAGE( "--- Test failure when account does not have required funds" );
      limit_order_create2_operation op;
      signed_transaction tx;

      op.owner = "bob";
      op.orderid = 1;
      op.amount_to_sell = ASSET( "10.000 2.28.0" );
      op.exchange_rate = price( ASSET( "1.000 2.28.0" ), ASSET( "1.000 2.28.2" ) );
      op.fill_or_kill = false;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( bob_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "bob", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "0.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "1000.0000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when price is 0" );

      op.owner = "alice";
      op.exchange_rate = price( ASSET( "0.000 2.28.0" ), ASSET( "1.000 2.28.2" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "1000.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure when amount to sell is 0" );

      op.amount_to_sell = ASSET( "0.000 2.28.0" );
      op.exchange_rate = price( ASSET( "1.000 2.28.0" ), ASSET( "1.000 2.28.2" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "1000.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test success creating limit order that will not be filled" );

      op.amount_to_sell = ASSET( "10.000 2.28.0" );
      op.exchange_rate = price( ASSET( "2.000 2.28.0" ), ASSET( "3.000 2.28.2" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      auto limit_order = limit_order_idx.find( std::make_tuple( "alice", op.orderid ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, op.owner );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, op.orderid );
      BOOST_REQUIRE( limit_order->for_sale == op.amount_to_sell.amount );
      BOOST_REQUIRE( limit_order->sell_price == op.exchange_rate );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test failure creating limit order with duplicate id" );

      op.amount_to_sell = ASSET( "20.000 2.28.0" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      limit_order = limit_order_idx.find( std::make_tuple( "alice", op.orderid ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, op.owner );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, op.orderid );
      BOOST_REQUIRE( limit_order->for_sale == 10000000 );
      BOOST_REQUIRE( limit_order->sell_price == op.exchange_rate );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test sucess killing an order that will not be filled" );

      op.orderid = 2;
      op.fill_or_kill = true;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test having a partial match to limit order" );
      // Alice has order for 15 SBD at a price of 2:3
      // Fill 5 MUSE for 7.5 SBD

      op.owner = "bob";
      op.orderid = 1;
      op.amount_to_sell = ASSET( "7.500000 2.28.2" );
      op.exchange_rate = price( ASSET( "3.000 2.28.2" ), ASSET( "2.000 2.28.0" ) );
      op.fill_or_kill = false;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      auto recent_ops = get_last_operations( 1 );
      auto fill_order_op = recent_ops[0].get< fill_order_operation >();

      limit_order = limit_order_idx.find( std::make_tuple( "alice", 1 ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, "alice" );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, op.orderid );
      BOOST_REQUIRE( limit_order->for_sale == 5000000 );
      BOOST_REQUIRE( limit_order->sell_price == price( ASSET( "2.000 2.28.0" ), ASSET( "3.000 2.28.2" ) ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "bob", op.orderid ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "7.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "5.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "992.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( fill_order_op.open_owner, "alice" );
      BOOST_REQUIRE_EQUAL( fill_order_op.open_orderid, 1 );
      BOOST_REQUIRE_EQUAL( fill_order_op.open_pays.amount.value, ASSET( "5.000 2.28.0").amount.value );
      BOOST_REQUIRE_EQUAL( fill_order_op.current_owner, "bob" );
      BOOST_REQUIRE_EQUAL( fill_order_op.current_orderid, 1 );
      BOOST_REQUIRE_EQUAL( fill_order_op.current_pays.amount.value, ASSET( "7.500000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test filling an existing order fully, but the new order partially" );

      op.amount_to_sell = ASSET( "15.000 2.28.2" );
      op.exchange_rate = price( ASSET( "3.000 2.28.2" ), ASSET( "2.000 2.28.0" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      limit_order = limit_order_idx.find( std::make_tuple( "bob", 1 ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, "bob" );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, 1 );
      BOOST_REQUIRE_EQUAL( limit_order->for_sale.value, 7500000 );
      BOOST_REQUIRE( limit_order->sell_price == price( ASSET( "3.000 2.28.2" ), ASSET( "2.000 2.28.0" ) ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", 1 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "990.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "15.000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "10.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "977.500000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test filling an existing order and new order fully" );

      op.owner = "alice";
      op.orderid = 3;
      op.amount_to_sell = ASSET( "5.000 2.28.0" );
      op.exchange_rate = price( ASSET( "2.000 2.28.0" ), ASSET( "3.000 2.28.2" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", 3 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "bob", 1 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "985.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "22.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "15.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "977.500000 2.28.2" ).amount.value );
      validate_database();

      BOOST_TEST_MESSAGE( "--- Test filling limit order with better order when partial order is better." );

      op.owner = "alice";
      op.orderid = 4;
      op.amount_to_sell = ASSET( "10.000 2.28.0" );
      op.exchange_rate = price( ASSET( "1.000 2.28.0" ), ASSET( "1.100000 2.28.2" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      op.owner = "bob";
      op.orderid = 4;
      op.amount_to_sell = ASSET( "12.000 2.28.2" );
      op.exchange_rate = price( ASSET( "1.200000 2.28.2" ), ASSET( "1.000 2.28.0" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      limit_order = limit_order_idx.find( std::make_tuple( "bob", 4 ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE( limit_order_idx.find(std::make_tuple( "alice", 4 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, "bob" );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, 4 );
      BOOST_REQUIRE_EQUAL( limit_order->for_sale.value, 1000000 );
      BOOST_REQUIRE( limit_order->sell_price == op.exchange_rate );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "975.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "33.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "25.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "965.500000 2.28.2" ).amount.value );
      validate_database();

      limit_order_cancel_operation can;
      can.owner = "bob";
      can.orderid = 4;
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( can );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_TEST_MESSAGE( "--- Test filling limit order with better order when partial order is worse." );

      auto gpo = db.get_dynamic_global_properties();

      op.owner = "alice";
      op.orderid = 5;
      op.amount_to_sell = ASSET( "20.000 2.28.0" );
      op.exchange_rate = price( ASSET( "1.000 2.28.0" ), ASSET( "1.100000 2.28.2" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      op.owner = "bob";
      op.orderid = 5;
      op.amount_to_sell = ASSET( "12.000 2.28.2" );
      op.exchange_rate = price( ASSET( "1.200000 2.28.2" ), ASSET( "1.000 2.28.0" ) );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( bob_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      limit_order = limit_order_idx.find( std::make_tuple( "alice", 5 ) );
      BOOST_REQUIRE( limit_order != limit_order_idx.end() );
      BOOST_REQUIRE( limit_order_idx.find(std::make_tuple( "bob", 5 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( limit_order->seller, "alice" );
      BOOST_REQUIRE_EQUAL( limit_order->orderid, 5 );
      BOOST_REQUIRE_EQUAL( limit_order->for_sale.value, 9090910 );
      BOOST_REQUIRE( limit_order->sell_price == price( ASSET( "1.000 2.28.0" ), ASSET( "1.100000 2.28.2" ) ) );
      BOOST_REQUIRE( limit_order->get_market() == std::make_pair( MUSE_SYMBOL, MBD_SYMBOL ) );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "955.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "45.500000 2.28.2" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.balance.amount.value, ASSET( "35.909090 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( bob.mbd_balance.amount.value, ASSET( "954.500000 2.28.2" ).amount.value );
      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( limit_order_cancel_validate )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: limit_order_cancel_validate" );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( limit_order_cancel_authorities )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: limit_order_cancel_authorities" );

      ACTORS( (alice)(bob) )
      fund( "alice", 10000000 );

      limit_order_create_operation c;
      c.owner = "alice";
      c.orderid = 1;
      c.amount_to_sell = ASSET( "1.000 2.28.0" );
      c.min_to_receive = ASSET( "1.000 2.28.2" );

      signed_transaction tx;
      tx.operations.push_back( c );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      limit_order_cancel_operation op;
      op.owner = "alice";
      op.orderid = 1;

      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "--- Test failure when no signature." );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      BOOST_TEST_MESSAGE( "--- Test success with account signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, database::skip_transaction_dupe_check );

      BOOST_TEST_MESSAGE( "--- Test failure with duplicate signature" );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_duplicate_sig );

      BOOST_TEST_MESSAGE( "--- Test failure with incorrect signature" );
      tx.signatures.clear();
      tx.sign( alice_post_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, database::skip_transaction_dupe_check ), tx_missing_active_auth );

      validate_database();
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( limit_order_cancel_apply )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: limit_order_cancel_apply" );

      ACTORS( (alice) )
      fund( "alice", 10000000 );

      const auto& limit_order_idx = db.get_index_type< limit_order_index >().indices().get< by_account >();

      BOOST_TEST_MESSAGE( "--- Test cancel non-existent order" );

      limit_order_cancel_operation op;
      signed_transaction tx;

      op.owner = "alice";
      op.orderid = 5;
      tx.operations.push_back( op );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );

      BOOST_TEST_MESSAGE( "--- Test cancel order" );

      limit_order_create_operation create;
      create.owner = "alice";
      create.orderid = 5;
      create.amount_to_sell = ASSET( "5.000 2.28.0" );
      create.min_to_receive = ASSET( "7.500000 2.28.2" );
      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( create );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", 5 ) ) != limit_order_idx.end() );

      tx.operations.clear();
      tx.signatures.clear();
      tx.operations.push_back( op );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( limit_order_idx.find( std::make_tuple( "alice", 5 ) ) == limit_order_idx.end() );
      BOOST_REQUIRE_EQUAL( alice.balance.amount.value, ASSET( "10.000 2.28.0" ).amount.value );
      BOOST_REQUIRE_EQUAL( alice.mbd_balance.amount.value, ASSET( "0.000 2.28.2" ).amount.value );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( account_recovery )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing: account recovery" );

      generate_blocks( fc::time_point_sec(MUSE_HARDFORK_0_2_TIME) );

      ACTORS( (alice) );
      fund( "alice", 1000000000 );

      BOOST_TEST_MESSAGE( "Creating account bob with alice" );

      account_create_operation acc_create;
      acc_create.fee = ASSET( "10.000 2.28.0" );
      acc_create.creator = "alice";
      acc_create.new_account_name = "bob";
      acc_create.owner = authority( 1, generate_private_key( "bob_owner" ).get_public_key(), 1 );
      acc_create.active = authority( 1, generate_private_key( "bob_active" ).get_public_key(), 1 );
      acc_create.basic = authority( 1, generate_private_key( "bob_posting" ).get_public_key(), 1 );
      acc_create.memo_key = generate_private_key( "bob_memo" ).get_public_key();
      acc_create.json_metadata = "";


      signed_transaction tx;
      tx.operations.push_back( acc_create );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      account_update_operation acc_update;
      request_account_recovery_operation request;
      recover_account_operation recover;

      {
      const auto& bob = db.get_account( "bob" );
      BOOST_REQUIRE( bob.owner == acc_create.owner );


      BOOST_TEST_MESSAGE( "Changing bob's owner authority" );

      acc_update.account = "bob";
      acc_update.owner = authority( 1, generate_private_key( "bad_key" ).get_public_key(), 1 );
      acc_update.memo_key = acc_create.memo_key;
      acc_update.json_metadata = "";

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( acc_update );
      tx.sign( generate_private_key( "bob_owner" ), db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( bob.owner == acc_update.owner );


      BOOST_TEST_MESSAGE( "Creating recover request for bob with alice" );

      request.recovery_account = "alice";
      request.account_to_recover = "bob";
      request.new_owner_authority = authority( 1, generate_private_key( "new_key" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( request );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( bob.owner == acc_update.owner );


      BOOST_TEST_MESSAGE( "Recovering bob's account with original owner auth and new secret" );

      recover.account_to_recover = "bob";
      recover.new_owner_authority = request.new_owner_authority;
      recover.recent_owner_authority = acc_create.owner;

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( recover );
      tx.sign( generate_private_key( "bob_owner" ), db.get_chain_id() );
      tx.sign( generate_private_key( "new_key" ), db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( bob.owner == recover.new_owner_authority );


      BOOST_TEST_MESSAGE( "Creating new recover request for a bogus key" );

      request.new_owner_authority = authority( 1, generate_private_key( "foo bar" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( request );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );
      }

      generate_blocks( db.head_block_time() + MUSE_OWNER_UPDATE_LIMIT + fc::seconds(MUSE_BLOCK_INTERVAL) );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      {
      const auto& bob = db.get_account( "bob" );
      BOOST_TEST_MESSAGE( "Testing failure when bob does not have new authority" );

      recover.new_owner_authority = authority( 1, generate_private_key( "idontknow" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( recover );
      tx.sign( generate_private_key( "bob_owner" ), db.get_chain_id() );
      tx.sign( generate_private_key( "idontknow" ), db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      BOOST_REQUIRE( bob.owner == authority( 1, generate_private_key( "new_key" ).get_public_key(), 1 ) );


      BOOST_TEST_MESSAGE( "Testing failure when bob does not have old authority" );

      recover.recent_owner_authority = authority( 1, generate_private_key( "idontknow" ).get_public_key(), 1 );
      recover.new_owner_authority = authority( 1, generate_private_key( "foo bar" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( recover );
      tx.sign( generate_private_key( "foo bar" ), db.get_chain_id() );
      tx.sign( generate_private_key( "idontknow" ), db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      BOOST_REQUIRE( bob.owner == authority( 1, generate_private_key( "new_key" ).get_public_key(), 1 ) );


      BOOST_TEST_MESSAGE( "Testing using the same old owner auth again for recovery" );

      recover.recent_owner_authority = authority( 1, generate_private_key( "bob_owner" ).get_public_key(), 1 );
      recover.new_owner_authority = authority( 1, generate_private_key( "foo bar" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( recover );
      tx.sign( generate_private_key( "bob_owner" ), db.get_chain_id() );
      tx.sign( generate_private_key( "foo bar" ), db.get_chain_id() );
      db.push_transaction( tx, 0 );

      BOOST_REQUIRE( bob.owner == recover.new_owner_authority );

      BOOST_TEST_MESSAGE( "Creating a recovery request that will expire" );

      request.new_owner_authority = authority( 1, generate_private_key( "expire" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( request );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      const auto& request_idx = db.get_index_type< account_recovery_request_index >().indices();
      auto req_itr = request_idx.begin();

      BOOST_REQUIRE( req_itr->account_to_recover == "bob" );
      BOOST_REQUIRE( req_itr->new_owner_authority == authority( 1, generate_private_key( "expire" ).get_public_key(), 1 ) );
      BOOST_REQUIRE( req_itr->expires == db.head_block_time() + MUSE_ACCOUNT_RECOVERY_REQUEST_EXPIRATION_PERIOD );
      auto expires = req_itr->expires;
      ++req_itr;
      BOOST_REQUIRE( req_itr == request_idx.end() );

      generate_blocks( time_point_sec( expires - MUSE_BLOCK_INTERVAL ), true );
      }

      const auto& new_request_idx = db.get_index_type< account_recovery_request_index >().indices();
      BOOST_REQUIRE( new_request_idx.begin() != new_request_idx.end() );

      generate_block();

      BOOST_REQUIRE( new_request_idx.begin() == new_request_idx.end() );

      recover.new_owner_authority = authority( 1, generate_private_key( "expire" ).get_public_key(), 1 );
      recover.recent_owner_authority = authority( 1, generate_private_key( "bob_owner" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( recover );
      tx.set_expiration( db.head_block_time() );
      tx.sign( generate_private_key( "expire" ), db.get_chain_id() );
      tx.sign( generate_private_key( "bob_owner" ), db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      BOOST_REQUIRE( db.get_account( "bob" ).owner == authority( 1, generate_private_key( "foo bar" ).get_public_key(), 1 ) );

      BOOST_TEST_MESSAGE( "Expiring owner authority history" );

      acc_update.owner = authority( 1, generate_private_key( "new_key" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( acc_update );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( generate_private_key( "foo bar" ), db.get_chain_id() );
      db.push_transaction( tx, 0 );

      generate_blocks( db.head_block_time() + ( MUSE_OWNER_AUTH_RECOVERY_PERIOD - MUSE_ACCOUNT_RECOVERY_REQUEST_EXPIRATION_PERIOD ) );
      generate_block();

      request.new_owner_authority = authority( 1, generate_private_key( "last key" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( request );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      recover.new_owner_authority = request.new_owner_authority;
      recover.recent_owner_authority = authority( 1, generate_private_key( "bob_owner" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( recover );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( generate_private_key( "bob_owner" ), db.get_chain_id() );
      tx.sign( generate_private_key( "last key" ), db.get_chain_id() );
      MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::assert_exception );
      BOOST_REQUIRE( db.get_account( "bob" ).owner == authority( 1, generate_private_key( "new_key" ).get_public_key(), 1 ) );

      generate_blocks( db.head_block_time() + MUSE_OWNER_UPDATE_LIMIT + fc::seconds(MUSE_BLOCK_INTERVAL) );

      recover.recent_owner_authority = authority( 1, generate_private_key( "foo bar" ).get_public_key(), 1 );

      tx.operations.clear();
      tx.signatures.clear();

      tx.operations.push_back( recover );
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( generate_private_key( "foo bar" ), db.get_chain_id() );
      tx.sign( generate_private_key( "last key" ), db.get_chain_id() );
      db.push_transaction( tx, 0 );
      BOOST_REQUIRE( db.get_account( "bob" ).owner == authority( 1, generate_private_key( "last key" ).get_public_key(), 1 ) );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( change_recovery_account )
{
   try
   {
      BOOST_TEST_MESSAGE( "Testing change_recovery_account_operation" );

      ACTORS( (alice)(bob)(sam)(tyler) )

      auto change_recovery_account = [&]( const std::string& account_to_recover, const std::string& new_recovery_account )
      {
         change_recovery_account_operation op;
         op.account_to_recover = account_to_recover;
         op.new_recovery_account = new_recovery_account;

         signed_transaction tx;
         tx.operations.push_back( op );
         tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
         tx.sign( alice_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );
      };

      auto recover_account = [&]( const std::string& account_to_recover, const fc::ecc::private_key& new_owner_key, const fc::ecc::private_key& recent_owner_key )
      {
         recover_account_operation op;
         op.account_to_recover = account_to_recover;
         op.new_owner_authority = authority( 1, public_key_type( new_owner_key.get_public_key() ), 1 );
         op.recent_owner_authority = authority( 1, public_key_type( recent_owner_key.get_public_key() ), 1 );

         signed_transaction tx;
         tx.operations.push_back( op );
         tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
         tx.sign( recent_owner_key, db.get_chain_id() );
         // only Alice -> throw
         MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::exception );
         tx.signatures.clear();
         tx.sign( new_owner_key, db.get_chain_id() );
         // only Sam -> throw
         MUSE_REQUIRE_THROW( db.push_transaction( tx, 0 ), fc::exception );
         tx.sign( recent_owner_key, db.get_chain_id() );
         // Alice+Sam -> OK
         db.push_transaction( tx, 0 );
      };

      auto request_account_recovery = [&]( const std::string& recovery_account, const fc::ecc::private_key& recovery_account_key, const std::string& account_to_recover, const public_key_type& new_owner_key )
      {
         request_account_recovery_operation op;
         op.recovery_account    = recovery_account;
         op.account_to_recover  = account_to_recover;
         op.new_owner_authority = authority( 1, new_owner_key, 1 );

         signed_transaction tx;
         tx.operations.push_back( op );
         tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
         tx.sign( recovery_account_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );
      };

      auto change_owner = [&]( const std::string& account, const fc::ecc::private_key& old_private_key, const public_key_type& new_public_key )
      {
         account_update_operation op;
         op.account = account;
         op.owner = authority( 1, new_public_key, 1 );

         signed_transaction tx;
         tx.operations.push_back( op );
         tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
         tx.sign( old_private_key, db.get_chain_id() );
         db.push_transaction( tx, 0 );
      };

      // if either/both users do not exist, we shouldn't allow it
      MUSE_REQUIRE_THROW( change_recovery_account("alice", "nobody"), fc::assert_exception );
      MUSE_REQUIRE_THROW( change_recovery_account("haxer", "sam"   ), fc::assert_exception );
      MUSE_REQUIRE_THROW( change_recovery_account("haxer", "nobody"), fc::assert_exception );
      change_recovery_account("alice", "sam");

      fc::ecc::private_key alice_priv1 = fc::ecc::private_key::regenerate( fc::sha256::hash( "alice_k1" ) );
      fc::ecc::private_key alice_priv2 = fc::ecc::private_key::regenerate( fc::sha256::hash( "alice_k2" ) );
      public_key_type alice_pub1 = public_key_type( alice_priv1.get_public_key() );
      public_key_type alice_pub2 = public_key_type( alice_priv2.get_public_key() );

      generate_blocks( db.head_block_time() + MUSE_OWNER_AUTH_RECOVERY_PERIOD - fc::seconds( MUSE_BLOCK_INTERVAL ), true );
      // cannot request account recovery until recovery account is approved
      MUSE_REQUIRE_THROW( request_account_recovery( "sam", sam_private_key, "alice", alice_pub1 ), fc::exception );
      generate_blocks(1);
      // cannot finish account recovery until requested
      MUSE_REQUIRE_THROW( recover_account( "alice", alice_priv1, alice_private_key ), fc::exception );
      // do the request
      request_account_recovery( "sam", sam_private_key, "alice", alice_pub1 );
      // can't recover with the current owner key
      MUSE_REQUIRE_THROW( recover_account( "alice", alice_priv1, alice_private_key ), fc::exception );
      // unless we change it!
      change_owner( "alice", alice_private_key, public_key_type( alice_pub2 ) );
      recover_account( "alice", alice_priv1, alice_private_key );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()
