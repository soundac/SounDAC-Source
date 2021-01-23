#pragma once
#include <fc/uint128.hpp>

#include <muse/chain/protocol/types.hpp>
#include <muse/chain/database.hpp>
#include <muse/chain/config.hpp>
#include <graphene/db/object.hpp>

namespace muse { namespace chain {


   /**
    * @class dynamic_global_property_object
    * @brief Maintains global state information
    * @ingroup object
    * @ingroup implementation
    *
    * This is an implementation detail. The values here are calculated during normal chain operations and reflect the
    * current values of global blockchain properties.
    */
   class dynamic_global_property_object : public abstract_object<dynamic_global_property_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_dynamic_global_property_object_type;

         uint32_t          head_block_number = 0;
         block_id_type     head_block_id;
         time_point_sec    time;
         string            current_witness;


         asset       virtual_supply             = asset( 0, MUSE_SYMBOL );
         asset       current_supply             = asset( 0, MUSE_SYMBOL );
         asset       confidential_supply        = asset( 0, MUSE_SYMBOL ); ///< total asset held in confidential balances
         asset       current_mbd_supply         = asset( 0, MBD_SYMBOL );
         asset       confidential_mbd_supply    = asset( 0, MBD_SYMBOL ); ///< total asset held in confidential balances
         asset       total_vesting_fund_muse    = asset( 0, MUSE_SYMBOL );
         asset       total_vesting_shares       = asset( 0, VESTS_SYMBOL );
         asset       total_reward_fund_muse     = asset( 0, MUSE_SYMBOL );
         asset       supply_delta               = asset( 0, MUSE_SYMBOL );


         uint32_t    maximum_proposal_lifetime = 86400;
         price       get_vesting_share_price() const
         {
            if ( total_vesting_fund_muse.amount == 0 || total_vesting_shares.amount == 0 )
               return price ( asset( 1000, MUSE_SYMBOL ), asset( 1000000, VESTS_SYMBOL ) );

            return price( total_vesting_shares, total_vesting_fund_muse );
         }

         /**
          *  This property defines the interest rate that SBD deposits receive.
          */
         uint16_t mbd_interest_rate = 0;

         /**
          *  Average block size is updated every block to be:
          *
          *     average_block_size = (99 * average_block_size + new_block_size) / 100
          *
          *  This property is used to update the current_reserve_ratio to maintain approximately
          *  50% or less utilization of network capacity.
          */
         uint32_t     average_block_size = 0;

         /**
          *  Maximum block size is decided by the set of active witnesses which change every round.
          *  Each witness posts what they think the maximum size should be as part of their witness
          *  properties, the median size is chosen to be the maximum block size for the round.
          *
          *  @note the minimum value for maximum_block_size is defined by the protocol to prevent the
          *  network from getting stuck by witnesses attempting to set this too low.
          */
         uint32_t     maximum_block_size = 0;

         /**
          * The current absolute slot number.  Equal to the total
          * number of slots since genesis.  Also equal to the total
          * number of missed slots plus head_block_number.
          */
         uint64_t      current_aslot = 0;

         /**
          * used to compute witness participation.
          */
         fc::uint128_t recent_slots_filled;
         uint8_t       participation_count; ///< Divide by 128 to compute participation percentage

         uint32_t last_irreversible_block_num = 0;

         /**
          * The maximum bandwidth the blockchain can support is:
          *
          *    max_bandwidth = maximum_block_size * MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS / MUSE_BLOCK_INTERVAL
          *
          * The maximum virtual bandwidth is:
          *
          *    max_bandwidth * current_reserve_ratio
          */
         uint64_t max_virtual_bandwidth = 0;

         /**
          *   Any time average_block_size <= 50% maximum_block_size this value grows by 1 until it
          *   reaches MUSE_MAX_RESERVE_RATIO.  Any time average_block_size is greater than
          *   50% it falls by 1%.  Upward adjustments happen once per round, downward adjustments
          *   happen every block.
          */
         uint64_t current_reserve_ratio = 1;

         uint32_t delegation_return_period = MUSE_DELEGATION_RETURN_PERIOD;

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

         /** The totoal amount of vesting shares (including delegation but not re-delegation) held by all
          *  streaming platforms.
          */
         share_type total_vested_by_platforms = 0;
   };
}}

FC_REFLECT_DERIVED( muse::chain::dynamic_global_property_object, (graphene::db::object),
                    (head_block_number)
                    (head_block_id)
                    (time)
                    (current_witness)
                    (virtual_supply)
                    (current_supply)
                    (confidential_supply)
                    (current_mbd_supply)
                    (confidential_mbd_supply)
                    (total_vesting_fund_muse)
                    (total_vesting_shares)
                    (total_reward_fund_muse)
                    (supply_delta)
                    (maximum_proposal_lifetime)
                    (mbd_interest_rate)
                    (average_block_size)
                    (maximum_block_size)
                    (current_aslot)
                    (recent_slots_filled)
                    (participation_count)
                    (last_irreversible_block_num)
                    (max_virtual_bandwidth)
                    (current_reserve_ratio)
                    (delegation_return_period)
                    (active_users)
                    (full_time_users)
                    (total_listening_time)
                    (full_users_time)
                    (total_vested_by_platforms)
                  )

