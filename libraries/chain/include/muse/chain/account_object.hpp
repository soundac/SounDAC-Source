#pragma once

#include <muse/chain/protocol/authority.hpp>
#include <muse/chain/protocol/types.hpp>
#include <muse/chain/protocol/base_operations.hpp>
#include <muse/chain/witness_objects.hpp>
#include <muse/chain/streaming_platform_objects.hpp>
  
#include <graphene/db/generic_index.hpp>

#include <boost/multi_index/composite_key.hpp>

#include <numeric>

namespace muse { namespace chain {

   class account_object : public abstract_object<account_object>
   {
      public:

         struct redelegation
         {
            uint16_t redelegate_pct = 0;
            share_type redelegated = 0;
         };

         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_account_object_type;

         string          name;
         authority       owner; ///< used for backup control, can set owner or active
         authority       active; ///< used for all monetary operations, can set active or basic
         authority       basic; ///< used for voting and friendship
         public_key_type memo_key;
         string          json_metadata = "";
         string          proxy;

         time_point_sec  last_owner_update;

         time_point_sec  created;
         bool            owner_challenged = false;
         bool            active_challenged = false;
         time_point_sec  last_owner_proved = time_point_sec::min();
         time_point_sec  last_active_proved = time_point_sec::min();
         string          recovery_account = "";
         time_point_sec  last_account_recovery;
         uint32_t        lifetime_vote_count = 0;

         uint64_t        score=0;
         
         uint32_t        total_listening_time = 0;
         map<streaming_platform_id_type,uint32_t> total_time_by_platform;
         uint16_t        voting_power = MUSE_100_PERCENT;   ///< current voting power of this account, it falls after every vote
         time_point_sec  last_vote_time; ///< used to increase the voting power of this account the longer it goes without voting.

         asset           balance = asset( 0, MUSE_SYMBOL );  ///< total liquid shares held by this account

         /**
          *  SBD Deposits pay interest based upon the interest rate set by witnesses. The purpose of these
          *  fields is to track the total (time * mbd_balance) that it is held. Then at the appointed time
          *  interest can be paid using the following equation:
          *
          *  interest = interest_rate * mbd_seconds / seconds_per_year
          *
          *  Every time the mbd_balance is updated the mbd_seconds is also updated. If at least
          *  MUSE_MIN_COMPOUNDING_INTERVAL_SECONDS has past since mbd_last_interest_payment then
          *  interest is added to mbd_balance.
          *
          *  @defgroup mbd_data mbd Balance Data
          */
         ///@{
         asset              mbd_balance = asset( 0, MBD_SYMBOL ); /// total mbd balance
         fc::uint128_t      mbd_seconds; ///< total mbd * how long it has been hel
         fc::time_point_sec mbd_seconds_last_update; ///< the last time the mbd_seconds was updated
         fc::time_point_sec mbd_last_interest_payment; ///< used to pay interest at most once per month
         ///@}

         asset           vesting_shares = asset( 0, VESTS_SYMBOL ); ///< total vesting shares held by this account, controls its voting power
         asset           delegated_vesting_shares = asset( 0, VESTS_SYMBOL );
         asset           received_vesting_shares = asset( 0, VESTS_SYMBOL );
         map<account_id_type,redelegation> redelegations;
         asset           redelegated_vesting_shares = asset( 0, VESTS_SYMBOL );
         asset           rereceived_vesting_shares = asset( 0, VESTS_SYMBOL );
         asset           vesting_withdraw_rate = asset( 0, VESTS_SYMBOL ); ///< at the time this is updated it can be at most vesting_shares/104
         time_point_sec  next_vesting_withdrawal = fc::time_point_sec::maximum(); ///< after every withdrawal this is incremented by 1 week
         share_type      withdrawn = 0; /// Track how many shares have been withdrawn
         share_type      to_withdraw = 0; /// Might be able to look this up with operation history.
         uint16_t        withdraw_routes = 0;

         std::vector<share_type> proxied_vsf_votes = std::vector<share_type>( MUSE_MAX_PROXY_RECURSION_DEPTH, 0 ); ///< the total VFS votes proxied to this account
         uint16_t        witnesses_voted_for = 0;

         uint16_t        streaming_platforms_voted_for = 0;

         /**
          *  This field tracks the average bandwidth consumed by this account and gets updated every time a transaction
          *  is produced by this account using the following equation. It has units of micro-bytes-per-second.
          *
          *  W = MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS = 1 week in seconds
          *  S = now - last_bandwidth_update
          *  N = fc::raw::packsize( transaction ) * 1,000,000
          *
          *  average_bandwidth = MIN(0,average_bandwidth * (W-S) / W) +  N * S / W
          *  last_bandwidth_update = T + S
          */
         uint64_t        average_bandwidth  = 0;
         uint64_t        lifetime_bandwidth = 0;
         time_point_sec  last_bandwidth_update;

         uint64_t        average_market_bandwidth  = 0;
         time_point_sec  last_market_bandwidth_update;

         /**
          *  Used to track activity rewards, updated on every post and comment
          */
         ///@{
         time_point_sec  last_active;
         ///@}


         account_id_type get_id()const { return id; }
         /// This function should be used only when the account votes for a witness directly
         share_type      witness_vote_weight()const {
            return std::accumulate( proxied_vsf_votes.begin(),
                                    proxied_vsf_votes.end(),
                                    vesting_shares.amount );
         }
         share_type      streaming_vote_weight()const {
            return vesting_shares.amount;
         }
         share_type      proxied_vsf_votes_total()const {
            return std::accumulate( proxied_vsf_votes.begin(),
                                    proxied_vsf_votes.end(),
                                    share_type() );
         }


         std::set<account_id_type> friends;
         std::set<account_id_type> second_level;
         std::set<account_id_type> waiting;

         uint64_t get_scoring_vesting() const { return vesting_shares.amount.value; }

   };


   /**
    *     * @brief Tracks the balance of a single account/asset pair
    *         * @ingroup object
    *             *
    *                 * This object is indexed on owner and asset_type so that black swan
    *                     * events in asset_type can be processed quickly.
    *                         */
   class account_balance_object : public abstract_object<account_balance_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_account_balance_object_type;
  
         account_id_type   owner;
         asset_id_type asset_type;
         share_type        balance;

         asset get_balance()const { return asset(balance, asset_type); }
         void  adjust_balance(const asset& delta){ assert(delta.asset_id == asset_type); balance += delta.amount;}
   };


   /**
    *  @brief This secondary index will allow a reverse lookup of all accounts that a particular key or account
    *  is an potential signing authority.
    */
   class account_member_index : public secondary_index
   {
      public:
         virtual void object_inserted( const object& obj ) override;
         virtual void object_removed( const object& obj ) override;
         virtual void about_to_modify( const object& before ) override;
         virtual void object_modified( const object& after  ) override;

         /** given an account or key, map it to the set of accounts that reference it in an active or owner authority */
         map< string, set<string> >          account_to_account_memberships;
         map< public_key_type, set<string> > account_to_key_memberships;

      protected:
         set<string>             get_account_members( const account_object& a )const;
         set<public_key_type>    get_key_members( const account_object& a )const;

         set<string>             before_account_members;
         set<public_key_type>    before_key_members;
   };

   class vesting_delegation_object : public abstract_object< vesting_delegation_object >
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_vesting_delegation_object_type;

         string            delegator;
         string            delegatee;
         asset             vesting_shares;
         time_point_sec    min_delegation_time;
   };

   class vesting_delegation_expiration_object : public abstract_object< vesting_delegation_expiration_object >
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_vesting_delegation_expiration_object_type;

         string            delegator;
         asset             vesting_shares;
         time_point_sec    expiration;
   };

   class owner_authority_history_object : public abstract_object< owner_authority_history_object >
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_owner_authority_history_object_type;

         string            account;
         authority         previous_owner_authority;
         time_point_sec    last_valid_time;

         owner_authority_history_id_type get_id()const { return id; }
   };

   class account_recovery_request_object : public abstract_object< account_recovery_request_object >
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_account_recovery_request_object_type;

         string 	      account_to_recover;
         authority      new_owner_authority;
         time_point_sec expires;

         account_recovery_request_id_type get_id()const { return id; }
   };

   class change_recovery_account_request_object : public abstract_object< change_recovery_account_request_object >
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_change_recovery_account_request_object_type;

         string         account_to_recover;
         string         recovery_account;
         time_point_sec effective_on;

         change_recovery_account_request_id_type get_id()const { return id; }
   };

   struct by_name;
   struct by_proxy;
   struct by_next_vesting_withdrawal;
   struct by_muse_balance;
   struct by_smp_balance;
   struct by_smd_balance;
   struct by_vote_count;
   struct by_last_owner_update;

   /**
    * @ingroup object_index
    */
   typedef multi_index_container<
      account_object,
      indexed_by<
         ordered_unique< tag< by_id >,
            member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_name >,
            member< account_object, string, &account_object::name > >,
         ordered_unique< tag< by_proxy >,
            composite_key< account_object,
               member< account_object, string, &account_object::proxy >,
               member<object, object_id_type, &object::id >
            > /// composite key by proxy
         >,
         ordered_unique< tag< by_next_vesting_withdrawal >,
            composite_key< account_object,
               member<account_object, time_point_sec, &account_object::next_vesting_withdrawal >,
               member<object, object_id_type, &object::id >
            > /// composite key by_next_vesting_withdrawal
         >,
         ordered_unique< tag< by_muse_balance >,
            composite_key< account_object,
               member<account_object, asset, &account_object::balance >,
               member<object, object_id_type, &object::id >
            >,
            composite_key_compare< std::greater< asset >, std::less< object_id_type > >
         >,
         ordered_unique< tag< by_smp_balance >,
            composite_key< account_object,
               member<account_object, asset, &account_object::vesting_shares >,
               member<object, object_id_type, &object::id >
            >,
            composite_key_compare< std::greater< asset >, std::less< object_id_type > >
         >,
         ordered_unique< tag< by_smd_balance >,
            composite_key< account_object,
               member<account_object, asset, &account_object::mbd_balance >,
               member<object, object_id_type, &object::id >
            >,
            composite_key_compare< std::greater< asset >, std::less< object_id_type > >
         >,
         ordered_unique< tag< by_vote_count >,
            composite_key< account_object,
               member<account_object, uint32_t, &account_object::lifetime_vote_count >,
               member<object, object_id_type, &object::id >
            >,
            composite_key_compare< std::greater< uint32_t >, std::less< object_id_type > >
         >,
         ordered_unique< tag< by_last_owner_update >,
            composite_key< account_object,
               member<account_object, time_point_sec, &account_object::last_owner_update >,
               member<object, object_id_type, &object::id >
            >,
            composite_key_compare< std::greater< time_point_sec >, std::less< object_id_type > >
         >
      >
   > account_multi_index_type;

   struct by_delegation;

   typedef multi_index_container <
      vesting_delegation_object,
      indexed_by <
         ordered_unique< tag< by_id >,
            member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_delegation >,
            composite_key< vesting_delegation_object,
               member< vesting_delegation_object, string, &vesting_delegation_object::delegator >,
               member< vesting_delegation_object, string, &vesting_delegation_object::delegatee >
            >,
            composite_key_compare< std::less< string >, std::less< string > >
         >
      >
   > vesting_delegation_multi_index_type;

   struct by_expiration;
   struct by_account_expiration;

   typedef multi_index_container <
      vesting_delegation_expiration_object,
      indexed_by <
         ordered_unique< tag< by_id >,
            member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_expiration >,
            composite_key< vesting_delegation_expiration_object,
               member< vesting_delegation_expiration_object, time_point_sec, &vesting_delegation_expiration_object::expiration >,
               member<object, object_id_type, &object::id >
            >,
            composite_key_compare< std::less< time_point_sec >, std::less< object_id_type > >
         >,
         ordered_unique< tag< by_account_expiration >,
            composite_key< vesting_delegation_expiration_object,
               member< vesting_delegation_expiration_object, string, &vesting_delegation_expiration_object::delegator >,
               member< vesting_delegation_expiration_object, time_point_sec, &vesting_delegation_expiration_object::expiration >,
               member<object, object_id_type, &object::id >
            >,
            composite_key_compare< std::less< string >, std::less< time_point_sec >, std::less< object_id_type > >
         >
      >
   > vesting_delegation_expiration_multi_index_type;

   struct by_account;
   struct by_last_valid;

   typedef multi_index_container <
      owner_authority_history_object,
      indexed_by <
         ordered_unique< tag< by_id >,
            member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_account >,
            composite_key< owner_authority_history_object,
               member< owner_authority_history_object, string, &owner_authority_history_object::account >,
               member< owner_authority_history_object, time_point_sec, &owner_authority_history_object::last_valid_time >,
               member< object, object_id_type, &object::id >
            >,
            composite_key_compare< std::less< string >, std::less< time_point_sec >, std::less< object_id_type > >
         >
      >
   > owner_authority_history_multi_index_type;

   typedef multi_index_container <
      account_recovery_request_object,
      indexed_by <
         ordered_unique< tag< by_id >,
            member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_account >,
            composite_key< account_recovery_request_object,
               member< account_recovery_request_object, string, &account_recovery_request_object::account_to_recover >,
               member< object, object_id_type, &object::id >
            >,
            composite_key_compare< std::less< string >, std::less< object_id_type > >
         >,
         ordered_unique< tag< by_expiration >,
            composite_key< account_recovery_request_object,
               member< account_recovery_request_object, time_point_sec, &account_recovery_request_object::expires >,
               member< object, object_id_type, &object::id >
            >,
            composite_key_compare< std::less< time_point_sec >, std::less< object_id_type > >
         >
      >
   > account_recovery_request_multi_index_type;

   struct by_effective_date;

   typedef multi_index_container <
      change_recovery_account_request_object,
      indexed_by <
         ordered_unique< tag< by_id >,
            member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_account >,
            composite_key< change_recovery_account_request_object,
               member< change_recovery_account_request_object, string, &change_recovery_account_request_object::account_to_recover >,
               member< object, object_id_type, &object::id >
            >,
            composite_key_compare< std::less< string >, std::less< object_id_type > >
         >,
         ordered_unique< tag< by_effective_date >,
            composite_key< change_recovery_account_request_object,
               member< change_recovery_account_request_object, time_point_sec, &change_recovery_account_request_object::effective_on >,
               member< object, object_id_type, &object::id >
            >,
            composite_key_compare< std::less< time_point_sec >, std::less< object_id_type > >
         >
      >
   > change_recovery_account_request_multi_index_type;

   typedef generic_index< account_object,                         account_multi_index_type >                         account_index;
   typedef generic_index< owner_authority_history_object,         owner_authority_history_multi_index_type >         owner_authority_history_index;
   typedef generic_index< account_recovery_request_object,        account_recovery_request_multi_index_type >        account_recovery_request_index;
   typedef generic_index< change_recovery_account_request_object, change_recovery_account_request_multi_index_type > change_recovery_account_request_index;
   typedef generic_index< vesting_delegation_object,              vesting_delegation_multi_index_type >              vesting_delegation_index;
   typedef generic_index< vesting_delegation_expiration_object,   vesting_delegation_expiration_multi_index_type >   vesting_delegation_expiration_index;

   struct by_account_asset;
   struct by_asset_balance;
   /**
    *     * @ingroup object_index
    *         */
   typedef multi_index_container <
      account_balance_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_unique< tag<by_account_asset>,
            composite_key<
               account_balance_object,
               member<account_balance_object, account_id_type, &account_balance_object::owner>,
               member<account_balance_object, asset_id_type, &account_balance_object::asset_type>
            >,
            composite_key_compare<
               std::less< account_id_type >,
               std::less < asset_id_type >
            >
         > ,
         ordered_unique< tag<by_asset_balance>,
            composite_key<
               account_balance_object,
               member<account_balance_object, asset_id_type, &account_balance_object::asset_type>,
               member<account_balance_object, share_type, &account_balance_object::balance>,
               member<account_balance_object, account_id_type, &account_balance_object::owner>
            >,
            composite_key_compare<
               std::less< asset_id_type >,
               std::greater< share_type >,
               std::less<account_id_type >
            >
         >
      >
   > account_balance_object_multi_index_type;
   typedef generic_index< account_balance_object, account_balance_object_multi_index_type >  account_balance_index;

}}

FC_REFLECT( muse::chain::account_object::redelegation, (redelegate_pct)(redelegated) )

FC_REFLECT_DERIVED( muse::chain::account_object, (graphene::db::object),
                    (name)(owner)(active)(basic)(memo_key)(json_metadata)(proxy)(last_owner_update)
                    (created)(total_listening_time)(total_time_by_platform)
                    (owner_challenged)(active_challenged)(last_owner_proved)(last_active_proved)(recovery_account)(last_account_recovery)
                    (lifetime_vote_count)(voting_power)(last_vote_time)
                    (balance)
                    (mbd_balance)(mbd_seconds)(mbd_seconds_last_update)(mbd_last_interest_payment)
                    (vesting_shares)(delegated_vesting_shares)(received_vesting_shares)
                    (redelegations)(redelegated_vesting_shares)(rereceived_vesting_shares)
                    (vesting_withdraw_rate)(next_vesting_withdrawal)(withdrawn)(to_withdraw)(withdraw_routes)
                    (score)
                    (proxied_vsf_votes)(witnesses_voted_for)(streaming_platforms_voted_for)
                    (average_bandwidth)(lifetime_bandwidth)(last_bandwidth_update)
                    (average_market_bandwidth)(last_market_bandwidth_update)
                    (last_active)
                    (friends)(second_level)(waiting)
                  )
FC_REFLECT_DERIVED( muse::chain::vesting_delegation_object, (graphene::db::object),
                     (delegator)(delegatee)(vesting_shares)(min_delegation_time) )

FC_REFLECT_DERIVED( muse::chain::vesting_delegation_expiration_object, (graphene::db::object),
                     (delegator)(vesting_shares)(expiration) )

FC_REFLECT_DERIVED( muse::chain::owner_authority_history_object, (graphene::db::object),
                     (account)(previous_owner_authority)(last_valid_time)
                  )

FC_REFLECT_DERIVED( muse::chain::account_recovery_request_object, (graphene::db::object),
                     (account_to_recover)(new_owner_authority)(expires)
                  )

FC_REFLECT_DERIVED( muse::chain::change_recovery_account_request_object, (graphene::db::object),
                     (account_to_recover)(recovery_account)(effective_on)
                  )
FC_REFLECT_DERIVED( muse::chain::account_balance_object, (graphene::db::object), 
                    (owner)(asset_type)(balance) 
                  )
