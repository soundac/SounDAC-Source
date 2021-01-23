#pragma once

#include <muse/chain/protocol/authority.hpp>
#include <muse/chain/protocol/types.hpp>
#include <muse/chain/protocol/base_operations.hpp>

#include <graphene/db/generic_index.hpp>

#include <boost/multi_index/composite_key.hpp>

namespace muse { namespace chain {

   using namespace graphene::db;

   class streaming_platform_object : public abstract_object<streaming_platform_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_streaming_platform_object_type;

         /** the account that has authority over this straming platform */
         string          owner;
         time_point_sec  created;
         string          url;

         /**
          *  The total votes for this streaming platform. 
          */
         share_type      votes;

         /** The number of users who have at least one streaming report in the
          *  last 24 hours
          */
         uint32_t active_users = 0;

         /** The number of users who have at least 1 hour worth of streaming
          *  reports in the last 24 hours
          */
         uint32_t full_time_users = 0;

         /** Total listening time within the past 24 hours, in seconds.
          */
         uint32_t total_listening_time = 0;

         /** Full user time within the past 24 hours, in seconds. Means sum of
          *  the total listening time of all users, capped at 1 hour for each
          *  user.
          */
         uint32_t full_users_time = 0;

         uint64_t total_anon_listening_time = 0;

         streaming_platform_id_type get_id()const { return id; }
   };

   class stream_report_request_object : public abstract_object<stream_report_request_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_stream_report_request_object_type;

         string   requestor;
         string   reporter;
         uint16_t reward_pct;

         streaming_platform_id_type get_id()const { return id; }
   };

   class streaming_platform_vote_object : public abstract_object<streaming_platform_vote_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_streaming_platform_vote_object_type;

         streaming_platform_id_type streaming_platform;
         account_id_type account;
   };

   class report_object : public abstract_object<report_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_report_object_type;

         streaming_platform_id_type streaming_platform;
         optional<account_id_type> consumer;
         optional<uint64_t> sp_user_id;
         content_id_type content;
         time_point_sec created;
         uint32_t play_time;
         optional<account_id_type> playlist_creator;
         optional<streaming_platform_id_type> spinning_platform;
         optional<uint16_t> reward_pct;
   };

   class streaming_platform_user_object : public abstract_object<streaming_platform_user_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_streaming_platform_user_object_type;

         streaming_platform_id_type streaming_platform;
         uint64_t                   sp_user_id;
         uint32_t                   total_listening_time = 0;
   };

  /**
    * @ingroup object_index
    */
   struct by_name;
   struct by_vote_name;
   typedef multi_index_container<
      streaming_platform_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_unique< tag<by_name>, member<streaming_platform_object, string, &streaming_platform_object::owner> >,
         ordered_unique< tag<by_vote_name>,
            composite_key< streaming_platform_object,
               member<streaming_platform_object, share_type, &streaming_platform_object::votes >,
               member<streaming_platform_object, string, &streaming_platform_object::owner >
            >,
            composite_key_compare< std::greater< share_type >, std::less< string > >
         >
      >
   > streaming_platform_multi_index_type;

   struct by_account_streaming_platform;
   struct by_streaming_platform_account;
   typedef multi_index_container<
      streaming_platform_vote_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_unique< tag<by_account_streaming_platform>,
            composite_key< streaming_platform_vote_object,
               member<streaming_platform_vote_object, account_id_type, &streaming_platform_vote_object::account >,
               member<streaming_platform_vote_object, streaming_platform_id_type, &streaming_platform_vote_object::streaming_platform >
            >,
            composite_key_compare< std::less< account_id_type >, std::less< streaming_platform_id_type > >
         >,
         ordered_unique< tag<by_streaming_platform_account>,
            composite_key< streaming_platform_vote_object,
               member<streaming_platform_vote_object, streaming_platform_id_type, &streaming_platform_vote_object::streaming_platform >,
               member<streaming_platform_vote_object, account_id_type, &streaming_platform_vote_object::account >
            >,
            composite_key_compare< std::less< streaming_platform_id_type >, std::less< account_id_type > >
         >
      > // indexed_by
   > streaming_platform_vote_multi_index_type;

   struct by_platforms;
   typedef multi_index_container<
      stream_report_request_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_unique< tag<by_platforms>,
            composite_key< stream_report_request_object,
               member<stream_report_request_object, string, &stream_report_request_object::requestor >,
               member<stream_report_request_object, string, &stream_report_request_object::reporter >
            >,
            composite_key_compare< std::less< string >, std::less< string > >
         >
      > // indexed_by
   > stream_report_request_multi_index_type;

   typedef generic_index< streaming_platform_object,         streaming_platform_multi_index_type>             streaming_platform_index;
   typedef generic_index< streaming_platform_vote_object,    streaming_platform_vote_multi_index_type >       streaming_platform_vote_index;
   typedef generic_index< stream_report_request_object,      stream_report_request_multi_index_type>          stream_report_request_index;
   
   struct by_consumer;
   struct by_content;
   struct by_streaming_platform;
   struct by_created;
   typedef multi_index_container<
      report_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_unique< tag<by_consumer>, 
            composite_key< report_object, 
               member< report_object, fc::optional<account_id_type>, &report_object::consumer >,
               member< object, object_id_type,  &object::id >
            >
         >,
         ordered_unique< tag<by_streaming_platform>, 
            composite_key< report_object,   
               member<report_object, streaming_platform_id_type, &report_object::streaming_platform>,
               member<object, object_id_type, &object::id >
            >
         >,
         ordered_unique< tag<by_created>,
            composite_key< report_object,
               member<report_object, time_point_sec,  &report_object::created>,
               member<object, object_id_type, &object::id >
            >
         >
      >
   > report_object_multi_index_type;
   typedef generic_index< report_object, report_object_multi_index_type > report_index;

   typedef multi_index_container<
      streaming_platform_user_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_unique< tag<by_consumer>,
            composite_key< streaming_platform_user_object,
               member< streaming_platform_user_object, streaming_platform_id_type, &streaming_platform_user_object::streaming_platform >,
               member< streaming_platform_user_object, uint64_t, &streaming_platform_user_object::sp_user_id >
            >
         >
      >
   > streaming_platform_user_object_multi_index_type;
   typedef generic_index< streaming_platform_user_object, streaming_platform_user_object_multi_index_type > streaming_platform_user_index;
} }

FC_REFLECT_DERIVED( muse::chain::streaming_platform_object, (graphene::db::object),
                    (owner)
                    (created)
                    (url)(votes)
                    (active_users)(full_time_users)(total_listening_time)(full_users_time)
                    (total_anon_listening_time)
                  )
FC_REFLECT_DERIVED( muse::chain::streaming_platform_vote_object, (graphene::db::object), (streaming_platform)(account) )

FC_REFLECT_DERIVED( muse::chain::stream_report_request_object, (graphene::db::object), (requestor)(reporter)(reward_pct) )

FC_REFLECT_DERIVED( muse::chain::report_object, (graphene::db::object), 
                    (streaming_platform)
                    (consumer)
                    (sp_user_id)
                    (content)
                    (created)
                    (play_time)
                    (playlist_creator)
                    (spinning_platform)
                    (reward_pct)
                  )

FC_REFLECT_DERIVED( muse::chain::streaming_platform_user_object, (graphene::db::object),
                    (streaming_platform)(sp_user_id)(total_listening_time) )
