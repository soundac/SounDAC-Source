/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 */
#pragma once
#include <muse/chain/evaluator.hpp>
#include <muse/chain/hardfork.hpp>
#include <muse/chain/node_property_object.hpp>
#include <muse/chain/fork_database.hpp>
#include <muse/chain/block_database.hpp>
#include <muse/chain/asset_object.hpp>
#include <muse/chain/balance_object.hpp>

#include <muse/chain/protocol/protocol.hpp>
#include <muse/chain/global_property_object.hpp>
#include <muse/chain/genesis_state.hpp>

#include <graphene/db/object_database.hpp>
#include <graphene/db/object.hpp>
#include <graphene/db/simple_index.hpp>
#include <fc/signals.hpp>

#include <fc/log/logger.hpp>

#include <map>

namespace muse { namespace chain {
   using graphene::db::abstract_object;
   using graphene::db::object;

   namespace detail{ uint32_t isqrt(uint64_t a); }
   /**
    *   @class database
    *   @brief tracks the blockchain state in an extensible manner
    */
   class database : public graphene::db::object_database
   {
      public:
         database();
         ~database();

         bool is_producing()const { return _is_producing; }
         void set_producing( bool p ) { _is_producing = p;  }
         bool _is_producing = false;

         enum validation_steps
         {
            skip_nothing                = 0,
            skip_witness_signature      = 1 << 0,  ///< used while reindexing
            skip_transaction_signatures = 1 << 1,  ///< used by non-witness nodes
            skip_transaction_dupe_check = 1 << 2,  ///< used while reindexing
            skip_fork_db                = 1 << 3,  ///< used while reindexing
            skip_block_size_check       = 1 << 4,  ///< used when applying locally generated transactions
            skip_tapos_check            = 1 << 5,  ///< used while reindexing -- note this skips expiration check as well
            skip_authority_check        = 1 << 6,  ///< used while reindexing -- disables any checking of authority on transactions
            skip_merkle_check           = 1 << 7,  ///< used while reindexing
            skip_undo_history_check     = 1 << 8,  ///< used while reindexing
            skip_witness_schedule_check = 1 << 9,  ///< used while reindexing
            skip_validate               = 1 << 10, ///< used prior to checkpoint, skips validate() call on transaction
            skip_validate_invariants    = 1 << 11  ///< used to skip database invariant check on block application
         };

         /**
          * @brief Open a database, creating a new one if necessary
          *
          * Opens a database in the specified directory. If no initialized database is found the database
          * will be initialized with the default state.
          *
          * @param data_dir Path to open or create database in
          * @param genesis_loader A callable object which returns the genesis state to initialize new databases on
          * @param db_version a version string that changes when the internal database format and/or logic is modified
          */
          void open( const fc::path& data_dir,
                     const genesis_state_type& initial_allocation,
                     const std::string& db_version );

         /**
          * @brief Rebuild object graph from block history and open detabase
          *
          * This method may be called after or instead of @ref database::open, and will rebuild the object graph by
          * replaying blockchain history. When this method exits successfully, the database will be open.
          */
         void reindex( fc::path data_dir );

         /**
          * @brief wipe Delete database from disk, and potentially the raw chain as well.
          * @param include_blocks If true, delete the raw chain as well as the database.
          *
          * Will close the database before wiping. Database will be closed when this function returns.
          */
         void wipe(const fc::path& data_dir, bool include_blocks);
         void close(bool rewind = true);

         //////////////////// db_block.cpp ////////////////////

         /**
          *  @return true if the block is in our fork DB or saved to disk as
          *  part of the official chain, otherwise return false
          */
         bool                       is_known_block( const block_id_type& id )const;
         bool                       is_known_transaction( const transaction_id_type& id )const;
         fc::sha256                 get_pow_target()const;
         block_id_type              get_block_id_for_num( uint32_t block_num )const;
         optional<signed_block>     fetch_block_by_id( const block_id_type& id )const;
         optional<signed_block>     fetch_block_by_number( uint32_t num )const;
         const signed_transaction&  get_recent_transaction( const transaction_id_type& trx_id )const;
         std::vector<block_id_type> get_block_ids_on_fork(block_id_type head_of_fork) const;

         chain_id_type              get_chain_id()const;
         const fc::sha256&          get_genesis_json_hash()const;

         const witness_object* find_witness( const string& name )const;
         const streaming_platform_object* find_streaming_platform( const string& name )const;
         const witness_object&  get_witness( const string& name )const;
         const streaming_platform_object & get_streaming_platform( const string& name) const;
         const account_object&  get_account( const string& name )const;
         const content_object&  get_content( const string& url )const;
         
         const escrow_object&   get_escrow( const string& name, uint32_t escrowid )const;
         const limit_order_object& get_limit_order( const string& owner, uint32_t id )const;
         const limit_order_object* find_limit_order( const string& owner, uint32_t id )const;

         /**
          *  Deducts fee from the account and the share supply
          */
         void pay_fee( const account_object& a, asset fee );
         void update_account_bandwidth( const account_object& a, uint32_t trx_size );
         void update_account_market_bandwidth( const account_object& a, uint32_t trx_size );

         asset get_effective_vesting_shares( const account_object& account, asset_id_type vested_symbol )const;

         void max_bandwidth_per_share()const;

         /**
          *  Calculate the percent of block production slots that were missed in the
          *  past 128 blocks, not including the current block.
          */
         uint32_t witness_participation_rate()const;

         void                                   add_checkpoints( const flat_map<uint32_t,block_id_type>& checkpts );
         const flat_map<uint32_t,block_id_type> get_checkpoints()const { return _checkpoints; }
         bool                                   before_last_checkpoint()const;

         bool push_block( const signed_block& b, uint32_t skip = skip_nothing );
         void push_transaction( const signed_transaction& trx, uint32_t skip = skip_nothing );
         bool _push_block( const signed_block& b );
         void _push_transaction( const signed_transaction& trx );
         void push_proposal( const proposal_object& proposal );
         signed_block generate_block(
            const fc::time_point_sec when,
            const string& witness_owner,
            const fc::ecc::private_key& block_signing_private_key,
            uint32_t skip
            );
         signed_block _generate_block(
            const fc::time_point_sec when,
            const string& witness_owner,
            const fc::ecc::private_key& block_signing_private_key
            );

         void pop_block();
         void clear_pending();

         /**
          *  This method is used to track appied operations during the evaluation of a block, these
          *  operations should include any operation actually included in a transaction as well
          *  as any implied/virtual operations that resulted, such as filling an order.  The
          *  applied operations is cleared after applying each block and calling the block
          *  observers which may want to index these operations.
          *
          *  @return the op_id which can be used to set the result after it has finished being applied.
          *  @todo rename this method notify_pre_apply_operation( op )
          */
         void push_applied_operation( const operation& op );
         void notify_post_apply_operation( const operation& op );

         /**
          *  This signal is emitted for plugins to process every operation after it has been fully applied.
          */
         fc::signal<void(const operation_object&)> pre_apply_operation;
         fc::signal<void(const operation_object&)> post_apply_operation;

         /**
          *  This signal is emitted after all operations and virtual operation for a
          *  block have been applied but before the get_applied_operations() are cleared.
          *
          *  You may not yield from this callback because the blockchain is holding
          *  the write lock and may be in an "inconstant state" until after it is
          *  released.
          */
         fc::signal<void(const signed_block&)>           applied_block;

         /**
          * This signal is emitted any time a new transaction is added to the pending
          * block state.
          */
         fc::signal<void(const signed_transaction&)>     on_pending_transaction;

         /**
          *  Emitted After a block has been applied and committed.  The callback
          *  should not yield and should execute quickly.
          */
         fc::signal<void(const vector<object_id_type>&)> changed_objects;

         /** this signal is emitted any time an object is removed and contains a
          * pointer to the last value of every object that was removed.
          */
         fc::signal<void(const vector<const object*>&)>  removed_objects;

         //////////////////// db_witness_schedule.cpp ////////////////////

         /**
          * @brief Get the witness scheduled for block production in a slot.
          *
          * slot_num always corresponds to a time in the future.
          *
          * If slot_num == 1, returns the next scheduled witness.
          * If slot_num == 2, returns the next scheduled witness after
          * 1 block gap.
          *
          * Use the get_slot_time() and get_slot_at_time() functions
          * to convert between slot_num and timestamp.
          *
          * Passing slot_num == 0 returns MUSE_NULL_WITNESS
          */
         string get_scheduled_witness(uint32_t slot_num)const;

         /**
          * @brief Checks if given streaming_platform has been upgraded to the role
          *
          *
          */
         bool is_streaming_platform(string streaming_platform) const;
         

         /**
          * @brief Checks if given streaming_platform has been elected to the role
          *
          * The streaming platform is elected to the role if it is ranked in the 
          * first MUSE_MAX_VOTED_STREAMING_PLATFORMS streaming platforms by vote
          *
          */
         bool is_voted_streaming_platform(string streaming_platform) const;
         
         /**
          * Get the time at which the given slot occurs.
          *
          * If slot_num == 0, return time_point_sec().
          *
          * If slot_num == N for N > 0, return the Nth next
          * block-interval-aligned time greater than head_block_time().
          */
         fc::time_point_sec get_slot_time(uint32_t slot_num)const;

         /**
          * Get the last slot which occurs AT or BEFORE the given time.
          *
          * The return value is the greatest value N such that
          * get_slot_time( N ) <= when.
          *
          * If no such N exists, return 0.
          */
         uint32_t get_slot_at_time(fc::time_point_sec when)const;

         /** @return the MBD created and deposited to_account, may return MUSE if there is no median feed */
         asset create_mbd(const account_object &to_account, asset muse);
         asset create_vesting( const account_object& to_account, asset muse );

         void update_witness_schedule();

         string      to_pretty_string( const asset& a )const;
         void        adjust_balance( const account_object& a, const asset& delta );
         void        adjust_supply( const asset& delta, bool adjust_vesting = false );
         void        update_owner_authority( const account_object& account, const authority& owner_authority );

         asset       get_balance( const account_object& a, asset_id_type symbol )const;
         asset       get_balance( const string& aname, asset_id_type symbol )const { return get_balance( get_account(aname), symbol ); }

         uint64_t    get_scoring(const account_object& ao ) const;
         uint64_t    get_scoring(const content_object& co ) const;
         void recalculate_score(const account_object& ao );
         void recursive_recalculate_score(const account_object& ao, share_type delta );

         const asset_object& get_asset( const string& symbol )const;
         /** this updates the votes for witnesses and streaming_platforms as a result of account voting proxy changing */
         void adjust_proxied_witness_votes( const account_object& a,
                                            const std::array< share_type, MUSE_MAX_PROXY_RECURSION_DEPTH+1 >& delta,
                                            int depth = 0 );

         /** this updates the votes for all witnesses and streaming_platforms as a result of account VESTS changing */
         void adjust_proxied_witness_votes( const account_object& a, share_type delta, int depth = 0 );

         /** this is called by `adjust_proxied_witness_votes` when account proxy to self */
         void adjust_witness_votes( const account_object& a, share_type delta );

         /** this updates the vote of a single witness as a result of a vote being added or removed*/
         void adjust_witness_vote( const witness_object& obj, share_type delta );

         /** this is called by `adjust_proxied_witness_votes` when account proxy to self */
         void adjust_streaming_platform_votes( const account_object& a, share_type delta );

         /** this updates the vote of a single witness as a result of a vote being added or removed*/
         void adjust_streaming_platform_vote( const streaming_platform_object& obj, share_type delta );

         /** clears all vote records for a particular account but does not update the
          * witness vote totals.  Vote totals should be updated first via a call to
          * adjust_proxied_witness_votes( a, -a.witness_vote_weight() )
          */
         void clear_witness_votes( const account_object& a );
         void clear_streaming_platform_votes( const account_object& a );
        
         vector<string> get_voted_streaming_platforms();
         void process_vesting_withdrawals();

         asset pay_to_content(const content_object & content, asset payout, streaming_platform_id_type platform);
         void pay_to_content_master(const content_object &content, const asset& payout);
         void pay_to_content_comp(const content_object &content, const asset& payout);

         asset process_content_cashout(const asset& content_reward);
         void process_funds(const asset& content_reward, const asset& witness_pay, const asset& vesting_reward);
         void adjust_funds(const asset& content_reward, const asset& paid_to_content);
         void process_conversions();
         void account_recovery_processing();
         void update_median_feed();
         share_type claim_rshare_reward( share_type rshares, uint16_t reward_weight, asset max_muse );

         asset get_content_reward()const;
         asset get_curation_reward()const;


         //////////////////// db_getter.cpp ////////////////////

         const dynamic_global_property_object&  get_dynamic_global_properties()const;
         const node_property_object&            get_node_properties()const;
         const feed_history_object&             get_feed_history()const;
         const witness_schedule_object&         get_witness_schedule_object()const;

         /**
          * Helper method to return the current mbd value of a given amount of
          * MUSE.  Return 0 SBD if there isn't a current_median_history
          */
         asset to_mbd( const asset& muse )const;
         asset to_muse(const asset &mbd)const;

         time_point_sec   head_block_time()const;
         uint32_t         head_block_num()const;
         block_id_type    head_block_id()const;

         node_property_object& node_properties();

         uint32_t last_non_undoable_block_num() const;
         //////////////////// db_init.cpp ////////////////////

         void initialize_evaluators();
         /// Reset the object graph in-memory
         void initialize_indexes();
         void init_genesis( const genesis_state_type& initial_allocation = genesis_state_type() );

         template<typename EvaluatorType>
         void register_evaluator()
         {
            _operation_evaluators[
               operation::tag<typename EvaluatorType::operation_type>::value].reset( new op_evaluator_impl<EvaluatorType>() );
         }

         /**
          *  This method validates transactions without adding it to the pending state.
          *  @throw if an error occurs
          */
         void validate_transaction( const signed_transaction& trx );


         /** when popping a block, the transactions that were removed get cached here so they
          * can be reapplied at the proper time */
         std::deque< signed_transaction >       _popped_tx;


         bool apply_order( const limit_order_object& new_order_object );
         bool fill_order( const limit_order_object& order, const asset& pays, const asset& receives );
         void cancel_order( const limit_order_object& obj );
         int  match( const limit_order_object& bid, const limit_order_object& ask, const price& trade_price );

         void perform_vesting_share_split( uint32_t magnitude );
         void retally_witness_vote_counts( bool force = false );
         void retally_liquidity_weight();

         bool has_hardfork( uint32_t hardfork )const;

         /* For testing and debugging only. Given a hardfork
            with id N, applies all hardforks with id <= N */
         void set_hardfork( uint32_t hardfork, bool process_now = true );

         void validate_invariants()const;
         /**
          * @}
          */

   protected:
         //Mark pop_undo() as protected -- we do not want outside calling pop_undo(); it should call pop_block() instead
         void pop_undo() { object_database::pop_undo(); }
         void notify_changed_objects();

      private:
         optional<undo_database::session>       _pending_tx_session;
         vector< unique_ptr<op_evaluator> >     _operation_evaluators;


         void apply_block( const signed_block& next_block, uint32_t skip = skip_nothing );
         void apply_transaction( const signed_transaction& trx, uint32_t skip = skip_nothing );
         void _apply_block( const signed_block& next_block );
         void _apply_transaction( const signed_transaction& trx );
         void apply_operation( transaction_evaluation_state& eval_state, const operation& op );


         ///Steps involved in applying a new block
         ///@{

         const witness_object& validate_block_header( uint32_t skip, const signed_block& next_block )const;
         void create_block_summary(const signed_block& next_block);

         void update_witness_schedule4();
         void update_median_witness_props();

         void update_global_dynamic_data( const signed_block& b );
         void update_virtual_supply();
         void update_signing_witness(const witness_object& signing_witness, const signed_block& new_block);
         void update_last_irreversible_block();
         void clear_expired_transactions();
         void clear_expired_proposals();
         void clear_expired_orders();
         void clear_expired_delegations();
         void process_header_extensions( const signed_block& next_block );

         void reset_virtual_schedule_time();

         void init_hardforks();
         void process_hardforks();
         void apply_hardfork( uint32_t hardfork );

         asset get_producer_reward();
         asset get_vesting_reward()const;

         void pay_to_platform( streaming_platform_id_type platform, const asset& payout, const string& url );
         ///@}

         vector< signed_transaction >  _pending_tx;
         fork_database                 _fork_db;
         fc::time_point_sec            _hardfork_times[ MUSE_NUM_HARDFORKS + 1 ];
         hardfork_version              _hardfork_versions[ MUSE_NUM_HARDFORKS + 1 ];

         /**
          *  Note: we can probably store blocks by block num rather than
          *  block id because after the undo window is past the block ID
          *  is no longer relevant and its number is irreversible.
          *
          *  During the "fork window" we can cache blocks in memory
          *  until the fork is resolved.  This should make maintaining
          *  the fork tree relatively simple.
          */
         block_database   _block_id_to_block;

         transaction_id_type               _current_trx_id;
         uint32_t                          _current_block_num    = 0;
         uint16_t                          _current_trx_in_block = 0;
         uint16_t                          _current_op_in_trx    = 0;
         uint16_t                          _current_virtual_op   = 0;

         flat_map<uint32_t,block_id_type>  _checkpoints;

         node_property_object              _node_property_object;

         fc::sha256                        genesis_json_hash;

         /**
          * Whether database is successfully opened or not.
          *
          * The database is considered open when there's no exception
          * or assertion fail during database::open() method, and
          * database::close() has not been called, or failed during execution.
          */
         bool                              _opened = false;

         // Counts nested proposal updates
         uint32_t                          _push_proposal_nesting_depth = 0;
   };
} }
