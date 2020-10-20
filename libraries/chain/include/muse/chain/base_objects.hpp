#pragma once

#include <muse/chain/protocol/authority.hpp>
#include <muse/chain/protocol/types.hpp>
#include <muse/chain/protocol/base_operations.hpp>
#include <muse/chain/witness_objects.hpp>
#include <muse/chain/proposal_object.hpp>
#include <muse/chain/streaming_platform_objects.hpp>
#include <muse/chain/asset_object.hpp>
#include <graphene/db/generic_index.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <muse/chain/content_object.hpp>

namespace muse { namespace chain {

   using namespace graphene::db;

   /**
    *  This object is used to track pending requests to convert mbd to muse
    */
   class convert_request_object : public abstract_object<convert_request_object> {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_convert_request_object_type;

         string         owner;
         uint32_t       requestid = 0; ///< id set by owner, the owner,requestid pair must be unique
         asset          amount;
         time_point_sec conversion_date; ///< at this time the feed_history_median_price * amount
   };

   class escrow_object : public abstract_object<escrow_object> {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_escrow_object_type;

         uint32_t       escrow_id;
         string         from;
         string         to;
         string         agent;
         time_point_sec expiration;
         asset          balance;
         bool           disputed = false;
   };



   /**
    *  If last_update is greater than 1 week, then volume gets reset to 0
    *
    *  When a user is a maker, their volume increases
    *  When a user is a taker, their volume decreases
    *
    *  Every 1000 blocks, the account that has the highest volume_weight() is paid the maximum of
    *  1000 MUSE or 1000 * virtual_supply / (100*blocks_per_year) aka 10 * virtual_supply / blocks_per_year
    *
    *  After being paid volume gets reset to 0
    */
   class liquidity_reward_balance_object : public abstract_object<liquidity_reward_balance_object> {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_liquidity_reward_balance_object_type;

         account_id_type owner;
         int64_t         muse_volume = 0;
         int64_t         mbd_volume = 0;
         uint128_t       weight = 0;

         /// this is the sort index
         uint128_t volume_weight()const { 
            return muse_volume * mbd_volume * is_positive(); 
        }
         uint128_t min_volume_weight()const { 
            return std::min(muse_volume,mbd_volume) * is_positive(); 
        }
         void update_weight( bool hf9 ) {
             weight = hf9 ? min_volume_weight() : volume_weight();
         }

         inline int is_positive()const { return ( muse_volume > 0 && mbd_volume > 0 ) ? 1 : 0; }

         time_point_sec last_update = fc::time_point_sec::min(); /// used to decay negative liquidity balances. block num
   };

   /**
    *  This object gets updated once per hour, on the hour
    */
   class feed_history_object  : public abstract_object<feed_history_object> {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_feed_history_object_type;

         price               actual_median_history; ///< the current actual median of the price history, used as the base for issuing MBD
         price               effective_median_history; ///< the base converting MBD back to MUSE
         std::deque<price>   price_history; ///< tracks this last week of median_feed one per hour
   };

   /**
    *  @brief an offer to sell a amount of a asset at a specified exchange rate by a certain time
    *  @ingroup object
    *  @ingroup protocol
    *  @ingroup market
    *
    *  This limit_order_objects are indexed by @ref expiration and is automatically deleted on the first block after expiration.
    */
   class limit_order_object : public abstract_object<limit_order_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_limit_order_object_type;

         time_point_sec   created;
         time_point_sec   expiration;
         string           seller;
         uint32_t         orderid;
         share_type       for_sale; ///< asset id is sell_price.base.symbol
         price            sell_price;

         pair<asset_id_type,asset_id_type> get_market()const
         {
            return sell_price.base.asset_id < sell_price.quote.asset_id ?
                std::make_pair(sell_price.base.asset_id, sell_price.quote.asset_id) :
                std::make_pair(sell_price.quote.asset_id, sell_price.base.asset_id);
         }

         asset amount_for_sale()const   { return asset( for_sale, sell_price.base.asset_id ); }
         asset amount_to_receive()const { return amount_for_sale() * sell_price; }
   };

   /**
    * @breif a route to send withdrawn vesting shares.
    */
   class withdraw_vesting_route_object : public abstract_object< withdraw_vesting_route_object >
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_withdraw_vesting_route_object_type;

         account_id_type from_account;
         account_id_type to_account;
         uint16_t        percent;
         bool            auto_vest;
   };

   struct by_price;
   struct by_expiration;
   struct by_account;
   struct by_market;
   typedef multi_index_container<
      limit_order_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_non_unique< tag<by_expiration>, member< limit_order_object, time_point_sec, &limit_order_object::expiration> >,
         ordered_unique< tag<by_price>,
            composite_key< limit_order_object,
               member< limit_order_object, price, &limit_order_object::sell_price>,
               member< object, object_id_type, &object::id>
            >,
            composite_key_compare< std::greater<price>, std::less<object_id_type> >
         >,
         ordered_unique< tag<by_account>,
            composite_key< limit_order_object,
               member< limit_order_object, string, &limit_order_object::seller>,
               member< limit_order_object, uint32_t, &limit_order_object::orderid>
            >
         >
      >
   > limit_order_multi_index_type;

   struct by_owner;
   struct by_conversion_date;
   typedef multi_index_container<
      convert_request_object,
      indexed_by<
         ordered_unique< tag< by_id >, member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_conversion_date >,
            composite_key< convert_request_object,
               member< convert_request_object, time_point_sec, &convert_request_object::conversion_date>,
               member< object, object_id_type, &object::id >
            >
         >,
         ordered_unique< tag< by_owner >,
            composite_key< convert_request_object,
               member< convert_request_object, string, &convert_request_object::owner>,
               member< convert_request_object, uint32_t, &convert_request_object::requestid >
            >
         >
      >
   > convert_request_index_type;

   struct by_owner;
   struct by_volume_weight;

   typedef multi_index_container<
      liquidity_reward_balance_object,
      indexed_by<
         ordered_unique< tag< by_id >, member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_owner >, member< liquidity_reward_balance_object, account_id_type, &liquidity_reward_balance_object::owner > >,
         ordered_unique< tag< by_volume_weight >,
            composite_key< liquidity_reward_balance_object,
                member< liquidity_reward_balance_object, fc::uint128, &liquidity_reward_balance_object::weight >,
                member< liquidity_reward_balance_object, account_id_type, &liquidity_reward_balance_object::owner >
            >,
            composite_key_compare< std::greater<fc::uint128>, std::less< account_id_type > >
         >
      >
   > liquidity_reward_balance_index_type;

   struct by_withdraw_route;
   typedef multi_index_container<
      withdraw_vesting_route_object,
      indexed_by<
         ordered_unique< tag< by_id >, member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_withdraw_route >,
            composite_key< withdraw_vesting_route_object,
               member< withdraw_vesting_route_object, account_id_type, &withdraw_vesting_route_object::from_account >,
               member< withdraw_vesting_route_object, account_id_type, &withdraw_vesting_route_object::to_account >
            >,
            composite_key_compare< std::less< account_id_type >, std::less< account_id_type > >
         >
      >
   > withdraw_vesting_route_index_type;

   struct by_from_id;
   struct by_to;
   struct by_agent;
   typedef multi_index_container<
      escrow_object,
      indexed_by<
         ordered_unique< tag< by_id >, member< object, object_id_type, &object::id > >,
         ordered_unique< tag< by_from_id >,
            composite_key< escrow_object,
               member< escrow_object, string,  &escrow_object::from >,
               member< escrow_object, uint32_t, &escrow_object::escrow_id >
            >
         >,
         ordered_unique< tag< by_to >,
            composite_key< escrow_object,
               member< escrow_object, string,  &escrow_object::to >,
               member< object, object_id_type, &object::id >
            >
         >,
         ordered_unique< tag< by_agent >,
            composite_key< escrow_object,
               member< escrow_object, string,  &escrow_object::agent >,
               member< object, object_id_type, &object::id >
            >
         >
      >
   > escrow_object_index_type;

   /**
    * @ingroup object_index
    */
   typedef generic_index< convert_request_object,              convert_request_index_type >              convert_index;
   typedef generic_index< limit_order_object,                  limit_order_multi_index_type >            limit_order_index;
   typedef generic_index< liquidity_reward_balance_object,     liquidity_reward_balance_index_type >     liquidity_reward_index;
   typedef generic_index< withdraw_vesting_route_object,       withdraw_vesting_route_index_type >       withdraw_vesting_route_index;
   typedef generic_index< escrow_object,                       escrow_object_index_type >                escrow_index;

} } // muse::chain

#include <muse/chain/account_object.hpp>


FC_REFLECT_DERIVED( muse::chain::limit_order_object, (graphene::db::object),
                    (created)(expiration)(seller)(orderid)(for_sale)(sell_price) )

FC_REFLECT_DERIVED( muse::chain::feed_history_object, (graphene::db::object),
                    (actual_median_history)(effective_median_history)(price_history) )

FC_REFLECT_DERIVED( muse::chain::convert_request_object, (graphene::db::object),
                    (owner)(requestid)(amount)(conversion_date) )

FC_REFLECT_DERIVED( muse::chain::liquidity_reward_balance_object, (graphene::db::object),
                    (owner)(muse_volume)(mbd_volume)(weight)(last_update) )

FC_REFLECT_DERIVED( muse::chain::withdraw_vesting_route_object, (graphene::db::object),
                    (from_account)(to_account)(percent)(auto_vest) )

FC_REFLECT_DERIVED( muse::chain::escrow_object, (graphene::db::object), 
                    (escrow_id)(from)(to)(agent)(expiration)(balance)(disputed) );
