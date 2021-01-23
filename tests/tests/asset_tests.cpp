/*
 * Copyright (c) 2018 Peertracks, Inc., and contributors.
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

#include <muse/chain/asset_object.hpp>
#include <muse/chain/protocol/asset_ops.hpp>
#include <muse/app/database_api.hpp>

#include "../common/database_fixture.hpp"

using namespace muse::chain;
using namespace graphene::db;

BOOST_FIXTURE_TEST_SUITE(asset_tests, clean_database_fixture)

BOOST_AUTO_TEST_CASE(create_asset_test)
{ try {
    ACTORS((bob)(federation));

    trx.clear();
    trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

    {
        asset_create_operation aco;
        aco.issuer = "federation";
        aco.symbol = "BTS";
        aco.precision = 5;
        aco.common_options.description = "IOU for BitShares core token";
        trx.operations.emplace_back(std::move(aco));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    const asset_object& bts = db.get_asset("BTS");
    BOOST_CHECK_EQUAL(0, bts.current_supply.value);
    BOOST_CHECK_EQUAL("BTS", bts.symbol_string);
    BOOST_CHECK_EQUAL(5, bts.precision);
    BOOST_CHECK(federation_id == bts.issuer);

    {
        asset_issue_operation aio;
        aio.issuer = "bob";
        aio.asset_to_issue = bts.amount(5000);
        aio.issue_to_account = "bob";
        trx.operations.emplace_back(std::move(aio));
        sign(trx, bob_private_key);
        BOOST_CHECK_THROW(PUSH_TX(db, trx), fc::assert_exception);
        trx.clear();

        aio.issuer = "federation";
        aio.asset_to_issue = bts.amount(100);
        aio.issue_to_account = "federation";
        trx.operations.emplace_back(std::move(aio));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();

        aio.issuer = "federation";
        aio.asset_to_issue = bts.amount(50);
        aio.issue_to_account = "bob";
        trx.operations.emplace_back(std::move(aio));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    asset amount = db.get_balance("federation", bts.id);
    BOOST_CHECK(bts.id == amount.asset_id);
    BOOST_CHECK_EQUAL(100, amount.amount.value);
    amount = db.get_balance("bob", bts.id);
    BOOST_CHECK(bts.id == amount.asset_id);
    BOOST_CHECK_EQUAL(50, amount.amount.value);

    {
        transfer_operation top;
        top.from = "federation";
        top.to = "bob";
        top.amount = bts.amount(10);
        trx.operations.emplace_back(std::move(top));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    amount = db.get_balance("federation", bts.id);
    BOOST_CHECK(bts.id == amount.asset_id);
    BOOST_CHECK_EQUAL(90, amount.amount.value);
    amount = db.get_balance("bob", bts.id);
    BOOST_CHECK(bts.id == amount.asset_id);
    BOOST_CHECK_EQUAL(60, amount.amount.value);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(trade_asset_test)
{ try {
    ACTORS((bob)(federation));
    fund("bob");
    
    // give bob some fake MBD
    generate_block();
    db.modify( bob_id(db), [] ( account_object& acct ) {
       acct.mbd_balance.amount = share_type(500000);
    });
    db.modify( db.get_dynamic_global_properties(), [] ( dynamic_global_property_object& gpo ) {
       gpo.current_mbd_supply.amount = share_type(500000);
    });
    db.modify( db.get_feed_history(), [] ( feed_history_object& fho ){
       fho.effective_median_history = fho.actual_median_history = asset(1) / asset(1, MBD_SYMBOL);
    });

    trx.clear();
    trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

    {
        asset_create_operation aco;
        aco.issuer = "federation";
        aco.symbol = "BTS";
        aco.precision = 5;
        aco.common_options.description = "IOU for BitShares core token";
        trx.operations.emplace_back(std::move(aco));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    const asset_object& bts = db.get_asset("BTS");
    {
        asset_issue_operation aio;
        aio.issuer = "federation";
        aio.asset_to_issue = bts.amount(500000);
        aio.issue_to_account = "federation";
        trx.operations.emplace_back(std::move(aio));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    {
        limit_order_create_operation loc;
        loc.owner = "federation";
        loc.amount_to_sell = bts.amount(100000);
        loc.min_to_receive = MBD_SYMBOL(db).amount(100000);
        trx.operations.emplace_back(std::move(loc));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    asset amount = db.get_balance("federation", bts.id);
    BOOST_CHECK(bts.id == amount.asset_id);
    BOOST_CHECK_EQUAL(400000, amount.amount.value);

    {
        limit_order_create_operation loc;
        loc.owner = "bob";
        loc.amount_to_sell = MBD_SYMBOL(db).amount(200000);
        loc.min_to_receive = bts.amount(200000);
        trx.operations.emplace_back(std::move(loc));
        sign(trx, bob_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    amount = db.get_balance("bob", bts.id);
    BOOST_CHECK(bts.id == amount.asset_id);
    BOOST_CHECK_EQUAL(100000, amount.amount.value);
    amount = db.get_balance("federation", MBD_SYMBOL);
    BOOST_CHECK(MBD_SYMBOL == amount.asset_id);
    BOOST_CHECK_EQUAL(100000, amount.amount.value);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE(trade_assets_test)
{ try {
    muse::app::database_api db_api( db );

    ACTORS( (alice)(bob)(federation));
    fund("alice");
    vest("alice", 50000);
    fund("bob", 5000000);
    vest("bob", 50000);

    trx.clear();
    trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

    {
        asset_create_operation aco;
        aco.issuer = "federation";
        aco.symbol = "BTS";
        aco.precision = 5;
        aco.common_options.description = "IOU for BitShares core token";
        trx.operations.emplace_back(aco);
        aco.symbol = "BTC";
        aco.precision = 8;
        aco.common_options.description = "IOU for Bitcoin";
        trx.operations.emplace_back(std::move(aco));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    const asset_object& bts = db.get_asset("BTS");
    const asset_object& btc = db.get_asset("BTC");

    {
        asset_issue_operation aio;
        aio.issuer = "federation";
        aio.asset_to_issue = bts.amount(5000000);
        aio.issue_to_account = "bob";
        trx.operations.emplace_back(aio);
        aio.asset_to_issue = btc.amount(500000);
        aio.issue_to_account = "alice";
        trx.operations.emplace_back(std::move(aio));
        sign(trx, federation_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    {
        limit_order_create_operation loc;
        loc.owner = "alice";
        loc.amount_to_sell = btc.amount(10000);
        loc.min_to_receive = MBD_SYMBOL(db).amount(30000000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = btc.amount(11000);
        loc.min_to_receive = MBD_SYMBOL(db).amount(34000000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = btc.amount(12000);
        loc.min_to_receive = MBD_SYMBOL(db).amount(38000000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = btc.amount(20000);
        loc.min_to_receive = MUSE_SYMBOL(db).amount(3000000000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = btc.amount(21000);
        loc.min_to_receive = MUSE_SYMBOL(db).amount(3250000000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = btc.amount(22000);
        loc.min_to_receive = MUSE_SYMBOL(db).amount(3500000000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = MUSE_SYMBOL(db).amount(1000);
        loc.min_to_receive = MBD_SYMBOL(db).amount(10);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = MUSE_SYMBOL(db).amount(1100);
        loc.min_to_receive = MBD_SYMBOL(db).amount(12);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = MUSE_SYMBOL(db).amount(1200);
        loc.min_to_receive = MBD_SYMBOL(db).amount(14);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        sign(trx, alice_private_key);
        PUSH_TX(db, trx);
        trx.clear();

        loc.owner = "bob";
        loc.amount_to_sell = bts.amount(100000);
        loc.min_to_receive = MBD_SYMBOL(db).amount(200000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = bts.amount(110000);
        loc.min_to_receive = MBD_SYMBOL(db).amount(230000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = bts.amount(120000);
        loc.min_to_receive = MBD_SYMBOL(db).amount(260000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = bts.amount(100000);
        loc.min_to_receive = MUSE_SYMBOL(db).amount(30000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = bts.amount(110000);
        loc.min_to_receive = MUSE_SYMBOL(db).amount(34000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = bts.amount(120000);
        loc.min_to_receive = MUSE_SYMBOL(db).amount(38000);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = MUSE_SYMBOL(db).amount(300000);
        loc.min_to_receive = btc.amount(20);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = MUSE_SYMBOL(db).amount(325000);
        loc.min_to_receive = btc.amount(21);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        loc.amount_to_sell = MUSE_SYMBOL(db).amount(350000);
        loc.min_to_receive = btc.amount(22);
        trx.operations.emplace_back(loc);
        loc.orderid++;
        sign(trx, bob_private_key);
        PUSH_TX(db, trx);
        trx.clear();
    }

    auto orderbook = db_api.get_order_book(1000);
    BOOST_CHECK_EQUAL("MUSE", orderbook.base);
    BOOST_CHECK_EQUAL("MBD", orderbook.quote);
    BOOST_CHECK(orderbook.bids.empty());
    BOOST_REQUIRE_EQUAL(3u, orderbook.asks.size());
    BOOST_CHECK(MBD_SYMBOL(db).amount(10) / MUSE_SYMBOL(db).amount(1000) == orderbook.asks[0].order_price);
    BOOST_CHECK_EQUAL(10, orderbook.asks[0].quote.value);
    BOOST_CHECK_EQUAL(1000, orderbook.asks[0].base.value);

    orderbook = db_api.get_order_book_for_asset(bts.id, 1000);
    BOOST_CHECK_EQUAL("BTS", orderbook.base);
    BOOST_CHECK_EQUAL("MBD", orderbook.quote);
    BOOST_CHECK(orderbook.bids.empty());
    BOOST_REQUIRE_EQUAL(3u, orderbook.asks.size());
    BOOST_CHECK(MBD_SYMBOL(db).amount(200000) / bts.amount(100000) == orderbook.asks[0].order_price);
    BOOST_CHECK_EQUAL(200000, orderbook.asks[0].quote.value);
    BOOST_CHECK_EQUAL(100000, orderbook.asks[0].base.value);

    orderbook = db_api.get_order_book_for_assets(btc.id, MUSE_SYMBOL, 1);
    BOOST_CHECK_EQUAL("BTC", orderbook.base);
    BOOST_CHECK_EQUAL("MUSE", orderbook.quote);
    BOOST_REQUIRE_EQUAL(1u, orderbook.bids.size());
    BOOST_CHECK(MUSE_SYMBOL(db).amount(350000) / btc.amount(22) == orderbook.bids[0].order_price);
    BOOST_CHECK_EQUAL(350000, orderbook.bids[0].quote.value);
    BOOST_CHECK_EQUAL(22, orderbook.bids[0].base.value);
    BOOST_REQUIRE_EQUAL(1u, orderbook.asks.size());
    BOOST_CHECK(MUSE_SYMBOL(db).amount(3000000000) / btc.amount(20000) == orderbook.asks[0].order_price);
    BOOST_CHECK_EQUAL(3000000000, orderbook.asks[0].quote.value);
    BOOST_CHECK_EQUAL(20000, orderbook.asks[0].base.value);

} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE( hardfork_test, database_fixture )
{ try {

   initialize_clean( 5 );

   ACTORS( (alice)(federation) );
   account_create( "federation.asset", federation_public_key );

   // Alice can create assets before HF 0.6
   asset_create_operation aco;
   aco.issuer = "alice";
   aco.symbol = "BTS";
   aco.precision = 5;
   aco.common_options.description = "IOU for BitShares core token";
   trx.operations.emplace_back(aco);
   sign(trx, alice_private_key);
   PUSH_TX(db, trx);
   trx.clear();

   generate_blocks( 2*MUSE_MAX_MINERS );
   generate_blocks( fc::time_point_sec( MUSE_HARDFORK_0_6_TIME + MUSE_BLOCK_INTERVAL ), true );
   trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );

   // ...but can't after
   aco.symbol = "BTC";
   trx.operations.emplace_back(aco);
   sign(trx, alice_private_key);
   BOOST_CHECK_THROW( PUSH_TX(db, trx), fc::assert_exception );
   trx.clear();

   // ...but federation can
   aco.issuer = "federation";
   trx.operations.emplace_back(aco);
   sign(trx, federation_private_key);
   PUSH_TX(db, trx);
   trx.clear();

   // ...and federation.asset can
   aco.issuer = "federation.asset";
   aco.symbol = "ETH";
   trx.operations.emplace_back(aco);
   sign(trx, federation_private_key);
   PUSH_TX(db, trx);
   trx.clear();
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE( softfork_test, database_fixture )
{ try {

   initialize_clean( 5 );

   ACTORS( (alice)(federation) );
   account_create( "federation.asset", federation_public_key );

   // Alice can create assets before HF 0.6
   asset_create_operation aco;
   aco.issuer = "alice";
   aco.symbol = "BTS";
   aco.precision = 5;
   aco.common_options.description = "IOU for BitShares core token";
   trx.operations.emplace_back(aco);
   sign(trx, alice_private_key);
   PUSH_TX(db, trx);
   trx.clear();

   // ...but it's not included in a block
   generate_block();
   BOOST_CHECK_THROW( db.get_asset("BTS"), fc::assert_exception );

   generate_blocks( fc::time_point::now() - fc::minutes(1) );
   generate_blocks( fc::time_point::now() - fc::seconds(25) );

   // A block containing it within the softfork window cannot be applied
   auto block = generate_block();
   BOOST_CHECK( fc::time_point::now() - fc::seconds(30) < fc::time_point(db.head_block_time()) );
   db.pop_block();
   trx.set_expiration( db.head_block_time() + MUSE_MAX_TIME_UNTIL_EXPIRATION );
   trx.operations.emplace_back( aco );
   sign( trx, alice_private_key );
   block.transactions.emplace_back( trx );
   block.transaction_merkle_root = block.calculate_merkle_root();
   block.sign( init_account_priv_key );
   BOOST_CHECK_THROW( db.push_block(block), fc::assert_exception );

   // ...but after the window has passed, it can
   if( block.timestamp + fc::seconds(31) > fc::time_point::now() )
      fc::usleep( block.timestamp + fc::seconds(31) - fc::time_point::now() );
   db.push_block(block);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()
