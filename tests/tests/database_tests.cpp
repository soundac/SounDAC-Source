/*
 * Copyright (c) 2017 Cryptonomex, Inc., and contributors.
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
#include <muse/chain/content_object.hpp>
#include <muse/chain/streaming_platform_objects.hpp>

#include <fc/crypto/digest.hpp>

#include "../common/database_fixture.hpp"

using namespace muse::chain;

BOOST_FIXTURE_TEST_SUITE( database_tests, database_fixture )

BOOST_AUTO_TEST_CASE( undo_test )
{
   try {
      database db;
      auto ses = db._undo_db.start_undo_session();
      const auto& sp_obj1 = db.create<streaming_platform_object>( [&]( streaming_platform_object& obj ){
          // no owner right now
      });
      auto id1 = sp_obj1.id;
      // abandon changes
      ses.undo();
      // start a new session
      ses = db._undo_db.start_undo_session();

      const auto& sp_obj2 = db.create<streaming_platform_object>( [&]( streaming_platform_object& obj ){
          // no owner right now
      });
      auto id2 = sp_obj2.id;
      BOOST_CHECK( id1 == id2 );
   } catch ( const fc::exception& e )
   {
      edump( (e.to_detail_string()) );
      throw;
   }
}

/**
 * Check that database modify() functors that throw do not get caught by boost, which will remove the object
 */
BOOST_AUTO_TEST_CASE(failed_modify_test)
{ try {
   database db;
   // Create dummy object
   const auto& obj = db.create<content_object>([](content_object& obj) {
                     obj.manage_master = authority( 1, "test", 1 );
                  });
   content_id_type obj_id = obj.id;
   BOOST_CHECK_EQUAL( 1, obj.manage_master.weight_threshold );

   // Modify dummy object, check that changes stick
   db.modify(obj, [](content_object& obj) {
      obj.manage_master = authority( 2, "tester", 2 );
   });
   BOOST_CHECK_EQUAL( 2, obj_id(db).manage_master.weight_threshold );

   // Throw exception when modifying object, check that object still exists after
   BOOST_CHECK_THROW(db.modify(obj, [](content_object& obj) {
      throw 5;
   }), fc::assert_exception);
   BOOST_CHECK_NE(db.find_object(obj_id), nullptr);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_CASE( merge_test )
{
   try {
      database db;
      auto ses = db._undo_db.start_undo_session();
      db.create<streaming_platform_object>( [&]( streaming_platform_object& obj ){
          obj.owner = "42";
      });
      ses.merge();

      auto sp = db.get_streaming_platform( "42" );
      BOOST_CHECK_EQUAL( "42", sp.owner );
   } catch ( const fc::exception& e )
   {
      edump( (e.to_detail_string()) );
      throw;
   }
}

BOOST_AUTO_TEST_SUITE_END()
