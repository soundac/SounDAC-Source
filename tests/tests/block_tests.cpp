/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <boost/test/unit_test.hpp>

#include <muse/chain/database.hpp>
#include <muse/chain/exceptions.hpp>
#include <muse/chain/base_objects.hpp>
#include <muse/chain/history_object.hpp>
#include <muse/account_history/account_history_plugin.hpp>

#include <graphene/utilities/tempdir.hpp>

#include <fc/crypto/digest.hpp>

#include "../common/database_fixture.hpp"

using namespace muse::chain;
using namespace muse::chain::test;

BOOST_AUTO_TEST_SUITE(block_tests)

BOOST_AUTO_TEST_CASE( block_database_test )
{
   try {
      fc::temp_directory data_dir( graphene::utilities::temp_directory_path() );

      block_database bdb;
      bdb.open( data_dir.path() );
      FC_ASSERT( bdb.is_open() );
      bdb.close();
      FC_ASSERT( !bdb.is_open() );
      bdb.open( data_dir.path() );

      signed_block b;
      for( uint32_t i = 0; i < 5; ++i )
      {
         if( i > 0 ) b.previous = b.id();
         b.witness = witness_id_type(i+1);
         bdb.store( b.id(), b );

         auto fetch = bdb.fetch_by_number( b.block_num() );
         FC_ASSERT( fetch.valid() );
         FC_ASSERT( fetch->witness ==  b.witness );
         fetch = bdb.fetch_by_number( i+1 );
         FC_ASSERT( fetch.valid() );
         FC_ASSERT( fetch->witness ==  b.witness );
         fetch = bdb.fetch_optional( b.id() );
         FC_ASSERT( fetch.valid() );
         FC_ASSERT( fetch->witness ==  b.witness );
      }

      for( uint32_t i = 1; i < 5; ++i )
      {
         auto blk = bdb.fetch_by_number( i );
         FC_ASSERT( blk.valid() );
         // Another ASSERT may be needed here
      }

      auto last = bdb.last();
      FC_ASSERT( last );
      FC_ASSERT( last->id() == b.id() );

      bdb.close();
      bdb.open( data_dir.path() );
      last = bdb.last();
      FC_ASSERT( last );
      FC_ASSERT( last->id() == b.id() );

      for( uint32_t i = 0; i < 5; ++i )
      {
         auto blk = bdb.fetch_by_number( i+1 );
         FC_ASSERT( blk.valid() );
         // Another ASSERT may be needed here
      }

   } catch (fc::exception& e) {
      edump((e.to_detail_string()));
      throw;
   }
}

static const fc::ecc::private_key& init_account_priv_key()
{
   static const auto priv_key = fc::ecc::private_key::regenerate( fc::sha256::hash( string( "init_key" ) ) );
   return priv_key;
}

static const fc::ecc::public_key& init_account_pub_key()
{
   static const auto pub_key = init_account_priv_key().get_public_key();
   return pub_key;
}

static void init_witness_keys( muse::chain::database& db )
{
   const account_object& init_acct = db.get_account( MUSE_INIT_MINER_NAME );
   db.modify( init_acct, []( account_object& acct ) {
      acct.active.add_authority( init_account_pub_key(), acct.active.weight_threshold );
   });
   const witness_object& init_witness = db.get_witness( MUSE_INIT_MINER_NAME );
   db.modify( init_witness, []( witness_object& witness ) {
      witness.signing_key = init_account_pub_key();
   });
}

BOOST_AUTO_TEST_CASE( generate_empty_blocks )
{
   try {
      fc::time_point_sec now( MUSE_TESTING_GENESIS_TIMESTAMP );
      fc::temp_directory data_dir( graphene::utilities::temp_directory_path() );
      signed_block b;

      genesis_state_type genesis;
      genesis.init_supply = INITIAL_TEST_SUPPLY;

      // TODO:  Don't generate this here
      signed_block cutoff_block;
      uint32_t last_block;
      {
         database db;
         db.open(data_dir.path(), genesis, "TEST" );
         init_witness_keys( db );
         b = db.generate_block(db.get_slot_time(1), db.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);

         // TODO:  Change this test when we correct #406
         // n.b. we generate MUSE_MIN_UNDO_HISTORY+1 extra blocks which will be discarded on save
         for( uint32_t i = 1; ; ++i )
         {
            BOOST_CHECK( db.head_block_id() == b.id() );
            //witness_id_type prev_witness = b.witness;
            string cur_witness = db.get_scheduled_witness(1);
            //BOOST_CHECK( cur_witness != prev_witness );
            b = db.generate_block(db.get_slot_time(1), cur_witness, init_account_priv_key(), database::skip_nothing);
            BOOST_CHECK( b.witness == cur_witness );
            uint32_t cutoff_height = db.get_dynamic_global_properties().last_irreversible_block_num;
            if( cutoff_height >= 200 )
            {
               cutoff_block = *(db.fetch_block_by_number( cutoff_height ));
               last_block = db.head_block_num();
               break;
            }
         }
         db.close();
      }
      {
         database db;
         db.open(data_dir.path(), genesis, "TEST" );
         BOOST_CHECK_EQUAL( db.head_block_num(), last_block );
         while( db.head_block_num() > cutoff_block.block_num() )
            db.pop_block();
         b = cutoff_block;
         for( uint32_t i = 0; i < 200; ++i )
         {
            BOOST_CHECK( db.head_block_id() == b.id() );
            //witness_id_type prev_witness = b.witness;
            string cur_witness = db.get_scheduled_witness(1);
            //BOOST_CHECK( cur_witness != prev_witness );
            b = db.generate_block(db.get_slot_time(1), cur_witness, init_account_priv_key(), database::skip_nothing);
         }
         BOOST_CHECK_EQUAL( db.head_block_num(), cutoff_block.block_num()+200 );
      }
   } catch (fc::exception& e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( undo_block )
{
   try {
      genesis_state_type genesis;
      genesis.init_supply = INITIAL_TEST_SUPPLY;

      fc::temp_directory data_dir( graphene::utilities::temp_directory_path() );
      {
         database db;
         db.open(data_dir.path(), genesis, "TEST" );
         init_witness_keys( db );
         fc::time_point_sec now( MUSE_TESTING_GENESIS_TIMESTAMP );
         std::vector< time_point_sec > time_stack;

         for( uint32_t i = 0; i < 5; ++i )
         {
            now = db.get_slot_time(1);
            time_stack.push_back( now );
            auto b = db.generate_block( now, db.get_scheduled_witness( 1 ), init_account_priv_key(), database::skip_nothing );
         }
         BOOST_CHECK( db.head_block_num() == 5 );
         BOOST_CHECK( db.head_block_time() == now );
         db.pop_block();
         time_stack.pop_back();
         now = time_stack.back();
         BOOST_CHECK( db.head_block_num() == 4 );
         BOOST_CHECK( db.head_block_time() == now );
         db.pop_block();
         time_stack.pop_back();
         now = time_stack.back();
         BOOST_CHECK( db.head_block_num() == 3 );
         BOOST_CHECK( db.head_block_time() == now );
         db.pop_block();
         time_stack.pop_back();
         now = time_stack.back();
         BOOST_CHECK( db.head_block_num() == 2 );
         BOOST_CHECK( db.head_block_time() == now );
         for( uint32_t i = 0; i < 5; ++i )
         {
            now = db.get_slot_time(1);
            time_stack.push_back( now );
            auto b = db.generate_block( now, db.get_scheduled_witness( 1 ), init_account_priv_key(), database::skip_nothing );
         }
         BOOST_CHECK( db.head_block_num() == 7 );
      }
   } catch (fc::exception& e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( fork_blocks )
{
   try {
      fc::temp_directory data_dir1( graphene::utilities::temp_directory_path() );
      fc::temp_directory data_dir2( graphene::utilities::temp_directory_path() );

      genesis_state_type genesis;
      genesis.init_supply = INITIAL_TEST_SUPPLY;

      //TODO This test needs 6-7 ish witnesses prior to fork

      database db1;
      db1.open( data_dir1.path(), genesis, "TEST" );
      init_witness_keys( db1 );
      database db2;
      db2.open( data_dir2.path(), genesis, "TEST" );
      init_witness_keys( db2 );

      BOOST_TEST_MESSAGE( "Adding blocks 1 through 10" );
      for( uint32_t i = 1; i <= 10; ++i )
      {
         auto b = db1.generate_block(db1.get_slot_time(1), db1.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
         try {
            PUSH_BLOCK( db2, b );
         } FC_CAPTURE_AND_RETHROW( ("db2") );
      }

      for( uint32_t j = 0; j <= 4; j += 4 )
      {
         // add blocks 11 through 13 to db1 only
         BOOST_TEST_MESSAGE( "Adding 3 blocks to db1 only" );
         for( uint32_t i = 11 + j; i <= 13 + j; ++i )
         {
            BOOST_TEST_MESSAGE( i );
            auto b = db1.generate_block(db1.get_slot_time(1), db1.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
         }
         string db1_tip = db1.head_block_id().str();

         // add different blocks 11 through 13 to db2 only
         BOOST_TEST_MESSAGE( "Add 3 different blocks to db2 only" );
         uint32_t next_slot = 3;
         for( uint32_t i = 11 + j; i <= 13 + j; ++i )
         {
            BOOST_TEST_MESSAGE( i );
            auto b =  db2.generate_block(db2.get_slot_time(next_slot), db2.get_scheduled_witness(next_slot), init_account_priv_key(), database::skip_nothing);
            next_slot = 1;
            // notify both databases of the new block.
            // only db2 should switch to the new fork, db1 should not
            PUSH_BLOCK( db1, b );
            BOOST_CHECK_EQUAL(db1.head_block_id().str(), db1_tip);
            BOOST_CHECK_EQUAL(db2.head_block_id().str(), b.id().str());
         }

         //The two databases are on distinct forks now, but at the same height.
         BOOST_CHECK_EQUAL(db1.head_block_num(), 13u + j);
         BOOST_CHECK_EQUAL(db2.head_block_num(), 13u + j);
         BOOST_CHECK( db1.head_block_id() != db2.head_block_id() );

         //Make a block on db2, make it invalid, then
         //pass it to db1 and assert that db1 doesn't switch to the new fork.
         signed_block good_block;
         {
            auto b = db2.generate_block(db2.get_slot_time(1), db2.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
            good_block = b;
            b.transactions.emplace_back(signed_transaction());
            b.transactions.back().operations.emplace_back(transfer_operation());
            b.sign( init_account_priv_key() );
            BOOST_CHECK_EQUAL(b.block_num(), 14u + j);
            MUSE_CHECK_THROW(PUSH_BLOCK( db1, b ), fc::exception);

            // At this point, `fetch_block_by_number` will fetch block from fork_db,
            //    so unable to reproduce the issue which is fixed in PR #938
            //    https://github.com/bitshares/bitshares-core/pull/938
            fc::optional<signed_block> previous_block = db1.fetch_block_by_number(1);
            BOOST_CHECK ( previous_block.valid() );
            uint32_t db1_blocks = db1.head_block_num();
            for( uint32_t curr_block_num = 2; curr_block_num <= db1_blocks; ++curr_block_num )
            {
               fc::optional<signed_block> curr_block = db1.fetch_block_by_number( curr_block_num );
               BOOST_CHECK( curr_block.valid() );
               BOOST_CHECK_EQUAL( curr_block->previous.str(), previous_block->id().str() );
               previous_block = curr_block;
            }
         }
         BOOST_CHECK_EQUAL(db1.head_block_num(), 13u + j);
         BOOST_CHECK_EQUAL(db1.head_block_id().str(), db1_tip);

         if( j == 0 )
         {
            // assert that db1 switches to new fork with good block
            BOOST_CHECK_EQUAL(db2.head_block_num(), 14u + j);
            PUSH_BLOCK( db1, good_block );
            BOOST_CHECK_EQUAL(db1.head_block_id().str(), db2.head_block_id().str());
         }
      }

      // generate more blocks to push the forked blocks out of fork_db
      BOOST_TEST_MESSAGE( "Adding more blocks to db1, push the forked blocks out of fork_db" );
      for( uint32_t i = 1; i <= 50; ++i )
      {
         db1.generate_block(db1.get_slot_time(1), db1.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
      }

      {
         // PR #938 make sure db is in a good state https://github.com/bitshares/bitshares-core/pull/938
         BOOST_TEST_MESSAGE( "Checking whether all blocks on disk are good" );
         fc::optional<signed_block> previous_block = db1.fetch_block_by_number(1);
         BOOST_CHECK ( previous_block.valid() );
         uint32_t db1_blocks = db1.head_block_num();
         for( uint32_t curr_block_num = 2; curr_block_num <= db1_blocks; ++curr_block_num )
         {
            fc::optional<signed_block> curr_block = db1.fetch_block_by_number( curr_block_num );
            BOOST_CHECK( curr_block.valid() );
            BOOST_CHECK_EQUAL( curr_block->previous.str(), previous_block->id().str() );
            previous_block = curr_block;
         }
      }
   } catch (fc::exception& e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( switch_forks_undo_create )
{
   try {
      fc::temp_directory dir1( graphene::utilities::temp_directory_path() ),
                         dir2( graphene::utilities::temp_directory_path() );

      genesis_state_type genesis;
      genesis.init_supply = INITIAL_TEST_SUPPLY;

      database db1,
               db2;
      db1.open( dir1.path(), genesis, "TEST" );
      init_witness_keys( db1 );
      db2.open( dir2.path(), genesis, "TEST" );
      init_witness_keys( db2 );

      const graphene::db::index& account_idx = db1.get_index(implementation_ids, impl_account_object_type);

      signed_transaction trx;
      account_id_type alice_id = account_idx.get_next_id();
      account_create_operation cop;
      cop.fee = asset(50, MUSE_SYMBOL);
      cop.new_account_name = "alice";
      cop.creator = MUSE_INIT_MINER_NAME;
      cop.owner = authority(1, init_account_pub_key(), 1);
      cop.active = cop.owner;
      trx.operations.push_back(cop);
      trx.set_expiration( db1.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      trx.sign( init_account_priv_key(), db1.get_chain_id() );
      PUSH_TX( db1, trx );

      // generate blocks
      // db1 : A
      // db2 : B C D
      auto b = db1.generate_block(db1.get_slot_time(1), db1.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);

      BOOST_CHECK( alice_id == db1.get_account( "alice" ).id );
      BOOST_CHECK( alice_id(db1).name == "alice" );

      b = db2.generate_block(db2.get_slot_time(1), db2.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
      db1.push_block(b);
      b = db2.generate_block(db2.get_slot_time(1), db2.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
      db1.push_block(b);
      MUSE_REQUIRE_THROW(alice_id(db2), fc::exception);
      alice_id(db1); /// it should be included in the pending state
      db1.clear_pending(); // clear it so that we can verify it was properly removed from pending state.
      MUSE_REQUIRE_THROW(alice_id(db1), fc::exception);

      PUSH_TX( db2, trx );

      b = db2.generate_block(db2.get_slot_time(1), db2.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
      db1.push_block(b);

      BOOST_CHECK(alice_id(db1).name == "alice");
      BOOST_CHECK(alice_id(db2).name == "alice");
   } catch (fc::exception& e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( duplicate_transactions )
{
   try {
      fc::temp_directory dir1( graphene::utilities::temp_directory_path() ),
                         dir2( graphene::utilities::temp_directory_path() );

      genesis_state_type genesis;
      genesis.init_supply = INITIAL_TEST_SUPPLY;

      database db1,
               db2;
      db1.open(dir1.path(), genesis, "TEST" );
      init_witness_keys( db1 );
      db2.open(dir2.path(), genesis, "TEST" );
      init_witness_keys( db2 );
      BOOST_CHECK( db1.get_chain_id() == db2.get_chain_id() );

      auto skip_sigs = database::skip_transaction_signatures | database::skip_authority_check;

      signed_transaction trx;
      account_create_operation cop;
      cop.new_account_name = "alice";
      cop.creator = MUSE_INIT_MINER_NAME;
      cop.owner = authority(1, init_account_pub_key(), 1);
      cop.active = cop.owner;
      trx.operations.push_back(cop);
      trx.set_expiration( db1.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      trx.sign( init_account_priv_key(), db1.get_chain_id() );
      PUSH_TX( db1, trx, skip_sigs );

      trx = decltype(trx)();
      transfer_operation t;
      t.from = MUSE_INIT_MINER_NAME;
      t.to = "alice";
      t.amount = asset(500,MUSE_SYMBOL);
      trx.operations.push_back(t);
      trx.set_expiration( db1.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      trx.sign( init_account_priv_key(), db1.get_chain_id() );
      PUSH_TX( db1, trx, skip_sigs );

      MUSE_CHECK_THROW(PUSH_TX( db1, trx, skip_sigs ), fc::exception);

      auto b = db1.generate_block( db1.get_slot_time(1), db1.get_scheduled_witness( 1 ), init_account_priv_key(), skip_sigs );
      PUSH_BLOCK( db2, b, skip_sigs );

      MUSE_CHECK_THROW(PUSH_TX( db1, trx, skip_sigs ), fc::exception);
      MUSE_CHECK_THROW(PUSH_TX( db2, trx, skip_sigs ), fc::exception);
      BOOST_CHECK_EQUAL(db1.get_balance( "alice", MUSE_SYMBOL ).amount.value, 500);
      BOOST_CHECK_EQUAL(db2.get_balance( "alice", MUSE_SYMBOL ).amount.value, 500);
   } catch (fc::exception& e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( tapos )
{
   try {
      fc::temp_directory dir1( graphene::utilities::temp_directory_path() );

      genesis_state_type genesis;
      genesis.init_supply = INITIAL_TEST_SUPPLY;

      database db1;
      db1.open(dir1.path(), genesis, "TEST" );
      init_witness_keys( db1 );

      auto b = db1.generate_block( db1.get_slot_time(1), db1.get_scheduled_witness( 1 ), init_account_priv_key(), database::skip_nothing);

      BOOST_TEST_MESSAGE( "Creating a transaction with reference block" );
      idump((db1.head_block_id()));
      signed_transaction trx;
      //This transaction must be in the next block after its reference, or it is invalid.
      trx.set_reference_block( db1.head_block_id() );

      account_create_operation cop;
      cop.fee = asset(50, MUSE_SYMBOL);
      cop.new_account_name = "alice";
      cop.creator = MUSE_INIT_MINER_NAME;
      cop.owner = authority(1, init_account_pub_key(), 1);
      cop.active = cop.owner;
      trx.operations.push_back(cop);
      trx.set_expiration( db1.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      trx.sign( init_account_priv_key(), db1.get_chain_id() );

      BOOST_TEST_MESSAGE( "Pushing Pending Transaction" );
      idump((trx));
      db1.push_transaction(trx);
      BOOST_TEST_MESSAGE( "Generating a block" );
      b = db1.generate_block(db1.get_slot_time(1), db1.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
      trx.clear();

      transfer_operation t;
      t.from = MUSE_INIT_MINER_NAME;
      t.to = "alice";
      t.amount = asset(50,MUSE_SYMBOL);
      trx.operations.push_back(t);
      trx.set_expiration( db1.head_block_time() + fc::seconds(2) );
      trx.sign( init_account_priv_key(), db1.get_chain_id() );
      idump((trx)(db1.head_block_time()));
      b = db1.generate_block(db1.get_slot_time(1), db1.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
      idump((b));
      b = db1.generate_block(db1.get_slot_time(1), db1.get_scheduled_witness(1), init_account_priv_key(), database::skip_nothing);
      trx.signatures.clear();
      trx.sign( init_account_priv_key(), db1.get_chain_id() );
      BOOST_REQUIRE_THROW( db1.push_transaction(trx, 0/*database::skip_transaction_signatures | database::skip_authority_check*/), fc::exception );
   } catch (fc::exception& e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_FIXTURE_TEST_CASE( optional_tapos, clean_database_fixture )
{
   try
   {
      idump((db.get_account("initminer")));
      ACTORS( (alice)(bob) );

      generate_block();

      BOOST_TEST_MESSAGE( "Create transaction" );

      transfer( MUSE_INIT_MINER_NAME, "alice", 1000000 );
      transfer_operation op;
      op.from = "alice";
      op.to = "bob";
      op.amount = asset(1000,MUSE_SYMBOL);
      signed_transaction tx;
      tx.operations.push_back( op );

      BOOST_TEST_MESSAGE( "ref_block_num=0, ref_block_prefix=0" );

      tx.ref_block_num = 0;
      tx.ref_block_prefix = 0;
      tx.signatures.clear();
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      PUSH_TX( db, tx );

      BOOST_TEST_MESSAGE( "proper ref_block_num, ref_block_prefix" );

      tx.signatures.clear();
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      PUSH_TX( db, tx, database::skip_transaction_dupe_check );

      BOOST_TEST_MESSAGE( "ref_block_num=0, ref_block_prefix=12345678" );

      tx.ref_block_num = 0;
      tx.ref_block_prefix = 0x12345678;
      tx.signatures.clear();
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( PUSH_TX( db, tx, database::skip_transaction_dupe_check ), fc::exception );

      BOOST_TEST_MESSAGE( "ref_block_num=1, ref_block_prefix=12345678" );

      tx.ref_block_num = 1;
      tx.ref_block_prefix = 0x12345678;
      tx.signatures.clear();
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( PUSH_TX( db, tx, database::skip_transaction_dupe_check ), fc::exception );

      BOOST_TEST_MESSAGE( "ref_block_num=9999, ref_block_prefix=12345678" );

      tx.ref_block_num = 9999;
      tx.ref_block_prefix = 0x12345678;
      tx.signatures.clear();
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      tx.sign( alice_private_key, db.get_chain_id() );
      MUSE_REQUIRE_THROW( PUSH_TX( db, tx, database::skip_transaction_dupe_check ), fc::exception );
   }
   catch (fc::exception& e)
   {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_FIXTURE_TEST_CASE( double_sign_check, clean_database_fixture )
{ try {
   generate_block();
   ACTOR(bob);
   share_type amount = 1000;

   transfer_operation t;
   t.from = MUSE_INIT_MINER_NAME;
   t.to = "bob";
   t.amount = asset(amount,MUSE_SYMBOL);
   trx.operations.push_back(t);
   trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
   trx.validate();

   db.push_transaction(trx, ~0);

   trx.operations.clear();
   account_witness_vote_operation v;
   v.account = "bob";
   v.witness = MUSE_INIT_MINER_NAME;
   trx.operations.push_back(v);
   trx.validate();

   BOOST_TEST_MESSAGE( "Verify that not-signing causes an exception" );
   MUSE_REQUIRE_THROW( db.push_transaction(trx, 0), fc::exception );

   BOOST_TEST_MESSAGE( "Verify that double-signing causes an exception" );
   trx.sign( bob_private_key, db.get_chain_id() );
   trx.sign( bob_private_key, db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction(trx, 0), tx_duplicate_sig );

   BOOST_TEST_MESSAGE( "Verify that signing with an extra, unused key fails" );
   trx.signatures.pop_back();
   trx.sign( generate_private_key("bogus" ), db.get_chain_id() );
   MUSE_REQUIRE_THROW( db.push_transaction(trx, 0), tx_irrelevant_sig );

   BOOST_TEST_MESSAGE( "Verify that signing once with the proper key passes" );
   trx.signatures.pop_back();
   db.push_transaction(trx, 0);

} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE( pop_block_twice, clean_database_fixture )
{
   try
   {
      uint32_t skip_flags = (
           database::skip_witness_signature
         | database::skip_transaction_signatures
         | database::skip_authority_check
         );

      // Sam is the creator of accounts
      private_key_type sam_key = generate_private_key( "sam" );
      account_object sam_account_object = account_create( "sam", sam_key.get_public_key() );

      //Get a sane head block time
      generate_block( skip_flags );

      transaction tx;
      signed_transaction ptx;

      // transfer from committee account to Sam account
      transfer( MUSE_INIT_MINER_NAME, "sam", 100000 );

      generate_block(skip_flags);

      account_create( "alice", generate_private_key( "alice" ).get_public_key() );
      generate_block(skip_flags);
      account_create( "bob", generate_private_key( "bob" ).get_public_key() );
      generate_block(skip_flags);

      db.pop_block();
      db.pop_block();
   } catch(const fc::exception& e) {
      edump( (e.to_detail_string()) );
      throw;
   }
}

BOOST_FIXTURE_TEST_CASE( rsf_missed_blocks, clean_database_fixture )
{
   try
   {
      generate_block();

      auto rsf = [&]() -> string
      {
         fc::uint128 rsf = db.get_dynamic_global_properties().recent_slots_filled;
         string result = "";
         result.reserve(128);
         for( int i=0; i<128; i++ )
         {
            result += ((rsf.lo & 1) == 0) ? '0' : '1';
            rsf >>= 1;
         }
         return result;
      };

      auto pct = []( uint32_t x ) -> uint32_t
      {
         return uint64_t( MUSE_100_PERCENT ) * x / 128;
      };

      BOOST_TEST_MESSAGE("checking initial participation rate" );
      BOOST_CHECK_EQUAL( rsf(),
         "1111111111111111111111111111111111111111111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), MUSE_100_PERCENT );

      BOOST_TEST_MESSAGE("Generating a block skipping 1" );
      generate_block( ~database::skip_fork_db, init_account_priv_key, 1 );
      BOOST_CHECK_EQUAL( rsf(),
         "0111111111111111111111111111111111111111111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(127) );

      BOOST_TEST_MESSAGE("Generating a block skipping 1" );
      generate_block( ~database::skip_fork_db, init_account_priv_key, 1 );
      BOOST_CHECK_EQUAL( rsf(),
         "0101111111111111111111111111111111111111111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(126) );

      BOOST_TEST_MESSAGE("Generating a block skipping 2" );
      generate_block( ~database::skip_fork_db, init_account_priv_key, 2 );
      BOOST_CHECK_EQUAL( rsf(),
         "0010101111111111111111111111111111111111111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(124) );

      BOOST_TEST_MESSAGE("Generating a block for skipping 3" );
      generate_block( ~database::skip_fork_db, init_account_priv_key, 3 );
      BOOST_CHECK_EQUAL( rsf(),
         "0001001010111111111111111111111111111111111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(121) );

      BOOST_TEST_MESSAGE("Generating a block skipping 5" );
      generate_block( ~database::skip_fork_db, init_account_priv_key, 5 );
      BOOST_CHECK_EQUAL( rsf(),
         "0000010001001010111111111111111111111111111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(116) );

      BOOST_TEST_MESSAGE("Generating a block skipping 8" );
      generate_block( ~database::skip_fork_db, init_account_priv_key, 8 );
      BOOST_CHECK_EQUAL( rsf(),
         "0000000010000010001001010111111111111111111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(108) );

      BOOST_TEST_MESSAGE("Generating a block skipping 13" );
      generate_block( ~database::skip_fork_db, init_account_priv_key, 13 );
      BOOST_CHECK_EQUAL( rsf(),
         "0000000000000100000000100000100010010101111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(95) );

      BOOST_TEST_MESSAGE("Generating a block skipping none" );
      generate_block();
      BOOST_CHECK_EQUAL( rsf(),
         "1000000000000010000000010000010001001010111111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(95) );

      BOOST_TEST_MESSAGE("Generating a block" );
      generate_block();
      BOOST_CHECK_EQUAL( rsf(),
         "1100000000000001000000001000001000100101011111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(95) );

      generate_block();
      BOOST_CHECK_EQUAL( rsf(),
         "1110000000000000100000000100000100010010101111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(95) );

      generate_block();
      BOOST_CHECK_EQUAL( rsf(),
         "1111000000000000010000000010000010001001010111111111111111111111"
         "1111111111111111111111111111111111111111111111111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(95) );

      generate_block( ~database::skip_fork_db, init_account_priv_key, 64 );
      BOOST_CHECK_EQUAL( rsf(),
         "0000000000000000000000000000000000000000000000000000000000000000"
         "1111100000000000001000000001000001000100101011111111111111111111"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(31) );

      generate_block( ~database::skip_fork_db, init_account_priv_key, 32 );
      BOOST_CHECK_EQUAL( rsf(),
         "0000000000000000000000000000000010000000000000000000000000000000"
         "0000000000000000000000000000000001111100000000000001000000001000"
      );
      BOOST_CHECK_EQUAL( db.witness_participation_rate(), pct(8) );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE( skip_block, clean_database_fixture )
{
   try
   {
      BOOST_TEST_MESSAGE( "Skipping blocks through db" );
      BOOST_REQUIRE( db.head_block_num() == 1 );

      int init_block_num = db.head_block_num();
      int miss_blocks = fc::minutes( 1 ).to_seconds() / MUSE_BLOCK_INTERVAL;
      auto witness = db.get_scheduled_witness( miss_blocks );
      auto block_time = db.get_slot_time( miss_blocks );
      db.generate_block( block_time , witness, init_account_priv_key, 0 );

      BOOST_CHECK_EQUAL( db.head_block_num(), init_block_num + 1 );
      BOOST_CHECK( db.head_block_time() == block_time );

      BOOST_TEST_MESSAGE( "Generating a block through fixture" );
      auto b = generate_block();

      BOOST_CHECK_EQUAL( db.head_block_num(), init_block_num + 2 );
      BOOST_CHECK( db.head_block_time() == block_time + MUSE_BLOCK_INTERVAL );
   }
   FC_LOG_AND_RETHROW();
}

BOOST_FIXTURE_TEST_CASE( hardfork_test, database_fixture )
{
   try
   {
      initialize_clean( 0 );

      generate_blocks( 2 * MUSE_MAX_MINERS );

      BOOST_TEST_MESSAGE( "Check hardfork not applied at genesis" );
      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_4 ) );

      BOOST_TEST_MESSAGE( "Generate blocks up to the hardfork time and check hardfork still not applied" );
      generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_1_TIME - MUSE_BLOCK_INTERVAL ), true );

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_4 ) );

      BOOST_TEST_MESSAGE( "Generate a block and check hardfork is applied" );
      generate_block();

      string op_msg = "Test: Hardfork applied";
      auto itr = db.get_index_type< account_history_index >().indices().get< by_id >().end();
      itr--;

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( get_last_operations( 1 )[0].get< custom_operation >().data == vector< char >( op_msg.begin(), op_msg.end() ) );
      BOOST_REQUIRE( itr->op(db).timestamp == db.head_block_time() );

      BOOST_TEST_MESSAGE( "Testing hardfork is only applied once" );
      generate_block();

      itr = db.get_index_type< account_history_index >().indices().get< by_id >().end();
      itr--;

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( get_last_operations( 1 )[0].get< custom_operation >().data == vector< char >( op_msg.begin(), op_msg.end() ) );
      BOOST_REQUIRE( itr->op(db).timestamp == db.head_block_time() - MUSE_BLOCK_INTERVAL );

      generate_blocks( MUSE_MAX_MINERS );
      generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_2_TIME - MUSE_BLOCK_INTERVAL ), true );

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_4 ) );

      generate_block();

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_4 ) );

      generate_blocks( 2*MUSE_MAX_MINERS );
      generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_3_TIME - MUSE_BLOCK_INTERVAL ), true );

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_4 ) );

      generate_block();

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_4 ) );

      generate_blocks( 2*MUSE_MAX_MINERS );
      generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_4_TIME - MUSE_BLOCK_INTERVAL ), true );

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_4 ) );

      generate_block();

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_4 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_5 ) );

      generate_blocks( 2*MUSE_MAX_MINERS );
      generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_5_TIME - MUSE_BLOCK_INTERVAL ), true );

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_4 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_5 ) );

      generate_block();

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_4 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_5 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_6 ) );

      generate_blocks( 2*MUSE_MAX_MINERS );
      generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_6_TIME - MUSE_BLOCK_INTERVAL ), true );

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_4 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_5 ) );
      BOOST_REQUIRE( !db.has_hardfork( MUSE_HARDFORK_0_6 ) );

      generate_block();

      BOOST_REQUIRE( db.has_hardfork( 0 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_1 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_2 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_3 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_4 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_5 ) );
      BOOST_REQUIRE( db.has_hardfork( MUSE_HARDFORK_0_6 ) );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_FIXTURE_TEST_CASE( skip_witness_on_empty_key, database_fixture )
{ try {

   initialize_clean(2);

   const auto skip_sigs = database::skip_transaction_signatures | database::skip_authority_check;

   generate_blocks( 2 * MUSE_MAX_MINERS );

   const witness_schedule_object& wso = witness_schedule_id_type()(db);

   std::set<string> witnesses( wso.current_shuffled_witnesses.begin(), wso.current_shuffled_witnesses.end() );
   BOOST_CHECK_EQUAL( MUSE_MAX_MINERS, witnesses.size() );

   {
      witness_update_operation wup;
      wup.block_signing_key = public_key_type();
      wup.url = "http://peertracks.com";
      wup.owner = MUSE_INIT_MINER_NAME;
      wup.fee = asset( MUSE_MIN_ACCOUNT_CREATION_FEE, MUSE_SYMBOL );
      trx.operations.push_back( wup );
      trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
      PUSH_TX( db, trx, skip_sigs );
   }

   generate_blocks( 2 * MUSE_MAX_MINERS );

   witnesses.clear();
   witnesses.insert( wso.current_shuffled_witnesses.begin(), wso.current_shuffled_witnesses.end() );
   BOOST_CHECK_EQUAL( MUSE_MAX_MINERS, witnesses.size() );

   generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_3_TIME ), true );
   generate_blocks( 2 * MUSE_MAX_MINERS );
    
   std::set<string> fewer_witnesses( wso.current_shuffled_witnesses.begin(), wso.current_shuffled_witnesses.end() );
   BOOST_CHECK_EQUAL( MUSE_MAX_MINERS - 1, fewer_witnesses.size() );

   for( const string& w : fewer_witnesses )
      witnesses.erase( w );

   BOOST_CHECK_EQUAL( 1, witnesses.size() );

   BOOST_CHECK_EQUAL( 1, witnesses.count( MUSE_INIT_MINER_NAME ) );

} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE( expire_proposals_on_hf3, database_fixture )
{ try {

   initialize_clean(2);

   ACTORS( (alice)(bob) );
   const asset_object& core = asset_id_type()(db);

   generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_3_TIME - 5 * 60 ) );

   transfer_operation transfer_op;
   transfer_op.from = "alice";
   transfer_op.to  = "bob";
   transfer_op.amount = core.amount(100);

   proposal_create_operation op;
   op.proposed_ops.emplace_back( transfer_op );
   op.expiration_time = db.head_block_time() + fc::minutes(1);
   trx.operations.push_back(op);
   trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
   PUSH_TX( db, trx );
   trx.clear();

   op.proposed_ops.clear();
   transfer_op.amount = core.amount(50);
   op.proposed_ops.emplace_back( transfer_op );
   op.expiration_time = db.head_block_time() + fc::minutes(2);
   trx.operations.push_back(op);
   PUSH_TX( db, trx );
   trx.clear();

   const auto& pidx = db.get_index_type<proposal_index>().indices().get<by_id>();
   BOOST_CHECK_EQUAL( 2, pidx.size() );
   const proposal_id_type pid1 = pidx.begin()->id;
   const proposal_id_type pid2 = std::next( pidx.begin() )->id;

   proposal_update_operation uop;
   uop.proposal = pid1;
   uop.active_approvals_to_add.insert("alice");
   trx.operations.push_back(uop);
   sign( trx, alice_private_key );
   PUSH_TX( db, trx );
   trx.clear();

   // Proposals failed to execute
   BOOST_CHECK_EQUAL( 2, pidx.size() );

   fund( "alice" );
   const auto original_balance = get_balance("alice").amount.value;

   generate_block();

   // Proposals failed to execute
   BOOST_CHECK_EQUAL( 2, pidx.size() );
   BOOST_CHECK_EQUAL( original_balance, get_balance("alice").amount.value );

   const proposal_object& proposal1 = pid1(db);
   BOOST_CHECK_EQUAL(proposal1.required_active_approvals.size(), 1);
   BOOST_CHECK_EQUAL(proposal1.available_active_approvals.size(), 1);
   BOOST_CHECK_EQUAL(proposal1.required_owner_approvals.size(), 0);
   BOOST_CHECK_EQUAL(proposal1.available_owner_approvals.size(), 0);
   BOOST_CHECK_EQUAL("alice", *proposal1.required_active_approvals.begin());

   const proposal_object& proposal2 = pid2(db);
   BOOST_CHECK_EQUAL(proposal2.required_active_approvals.size(), 1);
   BOOST_CHECK_EQUAL(proposal2.available_active_approvals.size(), 0);
   BOOST_CHECK_EQUAL(proposal2.required_owner_approvals.size(), 0);
   BOOST_CHECK_EQUAL(proposal2.available_owner_approvals.size(), 0);
   BOOST_CHECK_EQUAL("alice", *proposal2.required_active_approvals.begin());

   generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_3_TIME ) );
   generate_blocks( 2*MUSE_MAX_MINERS );

   // Proposals were removed
   BOOST_CHECK_EQUAL( 0, pidx.size() );

   // ...and did not execute
   BOOST_CHECK_EQUAL( original_balance, get_balance("alice").amount.value );

} FC_LOG_AND_RETHROW() }

static void generate_1_day_of_misses( database& db, const string& witness_to_skip )
{
   for( int blocks = 0; blocks < MUSE_BLOCKS_PER_DAY + 2 * MUSE_MAX_MINERS; blocks++ )
   {
      int next = 0;
      string next_witness;
      do
      {
         next_witness = db.get_scheduled_witness( ++next );
      } while( next_witness == witness_to_skip );
      db.generate_block(db.get_slot_time( next ), next_witness, init_account_priv_key(), 0);
   }
}

BOOST_FIXTURE_TEST_CASE( clear_witness_key, clean_database_fixture )
{ try {

   generate_blocks( 2 * MUSE_MAX_MINERS );

   auto witness = db.get_witness( MUSE_INIT_MINER_NAME );

   BOOST_CHECK_NE( std::string(public_key_type()), std::string(witness.signing_key) );

   generate_1_day_of_misses( db, MUSE_INIT_MINER_NAME );

   witness = db.get_witness( MUSE_INIT_MINER_NAME );

   BOOST_CHECK_GT( witness.total_missed, MUSE_BLOCKS_PER_DAY / MUSE_MAX_MINERS - 5 );
   BOOST_CHECK_LT( witness.last_confirmed_block_num, db.head_block_num() - MUSE_BLOCKS_PER_DAY );
   BOOST_CHECK_EQUAL( std::string(public_key_type()), std::string(witness.signing_key) );

} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE( generate_block_size, clean_database_fixture )
{
   try
   {
      generate_block();

      db.modify( db.get_dynamic_global_properties(), []( dynamic_global_property_object& gpo )
      {
         gpo.maximum_block_size = MUSE_MIN_BLOCK_SIZE_LIMIT;
      });

      signed_transaction tx;
      tx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

      transfer_operation op;
      op.from = MUSE_INIT_MINER_NAME;
      op.to = MUSE_TEMP_ACCOUNT;
      op.amount = asset( 1000, MUSE_SYMBOL );

      // tx without ops is 78 bytes (77 + 1 for length of ops vector))
      // op is 26 bytes (25 for op + 1 byte static variant tag)
      // total is 65182
      std::vector<char> raw_op = fc::raw::pack_to_vector( op, 255 );
      BOOST_CHECK_EQUAL( 25, raw_op.size() );
      BOOST_CHECK_EQUAL( raw_op.size(), fc::raw::pack_size( op ) );

      for( size_t i = 0; i < 2507; i++ )
      {
         tx.operations.push_back( op );
      }

      tx.sign( init_account_priv_key, db.get_chain_id() );
      db.push_transaction( tx, 0 );

      std::vector<char> raw_tx = fc::raw::pack_to_vector( tx, 255 );
      BOOST_CHECK_EQUAL( 65182+77+2, raw_tx.size() ); // 2 bytes for encoding # of ops
      BOOST_CHECK_EQUAL( raw_tx.size(), fc::raw::pack_size( tx ) );

      // Original generation logic only allowed 115 bytes for the header
      BOOST_CHECK_EQUAL( 115, fc::raw::pack_size( signed_block_header() ) + 4 );
      // We are targetting a size (minus header) of 65420 which creates a block of "size" 65535
      // This block will actually be larger because the header estimates is too small

      // Second transaction
      // We need a 80 (65420 - (65182+77+2) - (77+1) - 1) byte op. We need a 55 character memo (1 byte for length); 54 = 80 - 25 (old op) - 1 (tag)
      op.memo = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ123";
      raw_op = fc::raw::pack_to_vector( op, 255 );
      BOOST_CHECK_EQUAL( 80, raw_op.size() );
      BOOST_CHECK_EQUAL( raw_op.size(), fc::raw::pack_size( op ) );

      tx.clear();
      tx.operations.push_back( op );
      sign( tx, init_account_priv_key );
      db.push_transaction( tx, 0 );

      raw_tx = fc::raw::pack_to_vector( tx, 255 );
      BOOST_CHECK_EQUAL( 78+80+1, raw_tx.size() );
      BOOST_CHECK_EQUAL( raw_tx.size(), fc::raw::pack_size( tx ) );

      generate_block();
      auto head_block = db.fetch_block_by_number( db.head_block_num() );
      BOOST_CHECK_GE( 65535, fc::raw::pack_size( head_block ) );

      // The last transfer should have been delayed due to size
      BOOST_CHECK_EQUAL( 1, head_block->transactions.size() );
   }
   FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()
