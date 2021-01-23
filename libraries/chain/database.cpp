
#include <muse/chain/protocol/base_operations.hpp>

#include <muse/chain/block_summary_object.hpp>
#include <muse/chain/compound.hpp>
#include <muse/chain/database.hpp>
#include <muse/chain/db_with.hpp>
#include <muse/chain/exceptions.hpp>
#include <muse/chain/global_property_object.hpp>
#include <muse/chain/history_object.hpp>
#include <muse/chain/proposal_object.hpp>
#include <muse/chain/base_evaluator.hpp>
#include <muse/chain/base_objects.hpp>
#include <muse/chain/transaction_evaluation_state.hpp>
#include <muse/chain/transaction_object.hpp>
#include <muse/chain/balance_object.hpp>

#include <graphene/db/flat_index.hpp>

#include <fc/smart_ref_impl.hpp>
#include <fc/uint128.hpp>

#include <fc/io/fstream.hpp>

#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>

#define VIRTUAL_SCHEDULE_LAP_LENGTH  ( fc::uint128(uint64_t(-1)) )
#define VIRTUAL_SCHEDULE_LAP_LENGTH2 ( fc::uint128::max_value() )

using namespace muse::chain;


namespace muse { namespace chain {

using boost::container::flat_set;

// C++ requires that static class variables declared and initialized
// in headers must also have a definition in a single source file,
// else linker errors will occur [1].
//
// The purpose of this source file is to collect such definitions in
// a single place.
//
// [1] http://stackoverflow.com/questions/8016780/undefined-reference-to-static-constexpr-char


const uint8_t account_object::space_id;
const uint8_t account_object::type_id;

const uint8_t block_summary_object::space_id;
const uint8_t block_summary_object::type_id;

const uint8_t transaction_object::space_id;
const uint8_t transaction_object::type_id;

const uint8_t witness_object::space_id;
const uint8_t witness_object::type_id;

const uint8_t streaming_platform_object::space_id;
const uint8_t streaming_platform_object::type_id;

muse::chain::asset_id_type MUSE_SYMBOL=(muse::chain::asset_id_type(0));
muse::chain::asset_id_type VESTS_SYMBOL=(muse::chain::asset_id_type(1));
muse::chain::asset_id_type MBD_SYMBOL=(muse::chain::asset_id_type(2));
    

inline u256 to256( const fc::uint128& t ) {
   u256 v(t.hi);
   v <<= 64;
   v += t.lo;
   return v;
}


database::database()
{
   initialize_indexes();
   initialize_evaluators();
}

database::~database()
{
   clear_pending();
}

void database::open( const fc::path& data_dir, const genesis_state_type& initial_allocation,
                     const std::string& db_version )
{
   try
   {
      bool wipe_object_db = false;
      if( !fc::exists( data_dir / "db_version" ) )
         wipe_object_db = true;
      else
      {
         std::string version_string;
         fc::read_file_contents( data_dir / "db_version", version_string );
         wipe_object_db = ( version_string != db_version );
      }
      if( wipe_object_db ) {
         ilog("Wiping object_database due to missing or wrong version");
         object_database::wipe( data_dir );
         std::ofstream version_file( (data_dir / "db_version").generic_string().c_str(),
                                     std::ios::out | std::ios::binary | std::ios::trunc );
         version_file.write( db_version.c_str(), db_version.size() );
         version_file.close();
      }

      genesis_json_hash = initial_allocation.json_hash;
      ilog( "genesis.json hash is " + fc::string( genesis_json_hash ) );

      object_database::open(data_dir);

      _block_id_to_block.open(data_dir / "database" / "block_num_to_block");

      if( !find(dynamic_global_property_id_type()) )
         init_genesis( initial_allocation );

      init_hardforks();

      fc::optional<block_id_type> last_block = _block_id_to_block.last_id();
      if( last_block.valid() )
      {
         FC_ASSERT( *last_block >= head_block_id(),
                    "last block ID does not match current chain state",
                    ("last_block->id", last_block)("head_block_id",head_block_num()) );
         reindex( data_dir );
      }
   }
   FC_CAPTURE_LOG_AND_RETHROW( (data_dir) )
}

/** Cuts blocks from the end of the block database.
 *
 * @param blocks the block database from which to remove blocks
 * @param until the last block number to keep in the database
 */
static void cutoff_blocks( block_database& blocks, uint32_t until )
{
   uint32_t count = 0;
   fc::optional< block_id_type > last_id = blocks.last_id();
   while( last_id.valid() && block_header::num_from_id( *last_id ) > until )
   {
      blocks.remove( *last_id );
      count++;
      last_id = blocks.last_id();
   }
   wlog( "Dropped ${n} blocks from after the gap", ("n", count) );
}

/** Reads blocks number from start_block_num until last_block_num (inclusive)
 *  from the blocks database and pushes/applies them. Returns early if a block
 *  cannot be read from blocks.
 *  @return the number of the block following the last successfully read,
 *          usually last_block_num+1
 */
static uint32_t reindex_range( block_database& blocks, uint32_t start_block_num, uint32_t last_block_num,
        std::function<void( const signed_block& )> push_or_apply )
{
   for( uint32_t i = start_block_num; i <= last_block_num; ++i )
   {
      if( i % 100000 == 0 )
         ilog( "${pct}%   ${i} of ${n}", ("pct",double(i*100)/last_block_num)("i",i)("n",last_block_num) );
      fc::optional< signed_block > block = blocks.fetch_by_number(i);
      if( !block.valid() )
      {
         wlog( "Reindexing terminated due to gap:  Block ${i} does not exist!", ("i", i) );
         cutoff_blocks( blocks, i );
         return i;
      }
      push_or_apply( *block );
   }
   return last_block_num + 1;
};

void database::reindex( fc::path data_dir )
{
   try
   {
      auto last_block = _block_id_to_block.last();
      if( !last_block )
      {
         elog( "!no last block" );
         edump((last_block));
         return;
      }
      if( last_block->block_num() <= head_block_num()) return;

      ilog( "Replaying blocks..." );
      _undo_db.disable();

      auto start = fc::time_point::now();
      const uint32_t last_block_num_in_file = last_block->block_num();
      const uint32_t initial_undo_blocks = MUSE_MAX_UNDO_HISTORY;

      uint32_t first = head_block_num() + 1;
      if( last_block_num_in_file > 2 * initial_undo_blocks
          && first < last_block_num_in_file - 2 * initial_undo_blocks )
      {
         first = reindex_range( _block_id_to_block, first, last_block_num_in_file - 2 * initial_undo_blocks,
            [this]( const signed_block& block ) {
                apply_block( block, skip_witness_signature |
                                    skip_transaction_signatures |
                                    skip_transaction_dupe_check |
                                    skip_tapos_check |
                                    skip_witness_schedule_check |
                                    skip_authority_check |
                                    skip_validate | /// no need to validate operations
                                    skip_validate_invariants );
            } );
         if( first > last_block_num_in_file - 2 * initial_undo_blocks )
         {
            ilog( "Writing database to disk at block ${i}", ("i",first-1) );
            flush();
            ilog( "Done" );
         }
      }
      if( last_block_num_in_file > initial_undo_blocks
          && first < last_block_num_in_file - initial_undo_blocks )
      {
         first = reindex_range( _block_id_to_block, first, last_block_num_in_file - initial_undo_blocks,
            [this]( const signed_block& block ) {
                apply_block( block, skip_witness_signature |
                                    skip_transaction_signatures |
                                    skip_transaction_dupe_check |
                                    skip_tapos_check |
                                    skip_witness_schedule_check |
                                    skip_authority_check |
                                    skip_validate | /// no need to validate operations
                                    skip_validate_invariants );
            } );
      }
      if( first > 1 )
         _fork_db.start_block( *_block_id_to_block.fetch_by_number( first - 1 ) );
      _undo_db.enable();

      reindex_range( _block_id_to_block, first, last_block_num_in_file,
            [this]( const signed_block& block ) {
                push_block( block, skip_nothing );
            } );

      auto end = fc::time_point::now();
      ilog( "Done reindexing, elapsed time: ${t} sec", ("t",double((end-start).count())/1000000.0 ) );
   }
   FC_CAPTURE_AND_RETHROW( (data_dir) )

}

void database::wipe(const fc::path& data_dir, bool include_blocks)
{
   ilog("Wiping database", ("include_blocks", include_blocks));
   if (_opened) {
     close();
   }
   object_database::wipe(data_dir);
   if( include_blocks )
      fc::remove_all( data_dir / "database" );
}

void database::close(bool rewind)
{
   try
   {
      if( !_block_id_to_block.is_open() ) return;
      ilog( "Closing database" );

      // pop all of the blocks that we can given our undo history, this should
      // throw when there is no more undo history to pop
      if( rewind )
      {
         try
         {
            uint32_t cutoff = get_dynamic_global_properties().last_irreversible_block_num;

            clear_pending();
            while( head_block_num() > cutoff )
            {
               block_id_type popped_block_id = head_block_id();
               pop_block();
               _fork_db.remove(popped_block_id); // doesn't throw on missing
            }
         }
         catch ( const fc::exception& e )
         {
            ilog( "exception on rewind ${e}", ("e",e.to_detail_string()) );
         }
      }

      // Since pop_block() will move tx's in the popped blocks into pending,
      // we have to clear_pending() after we're done popping to get a clean
      // DB state (issue #336).
      clear_pending();

      object_database::flush();
      object_database::close();

      if( _block_id_to_block.is_open() )
         _block_id_to_block.close();

      _fork_db.reset();

      _opened = false;
   }
   FC_CAPTURE_AND_RETHROW()
}

bool database::is_known_block( const block_id_type& id )const
{
   return _fork_db.is_known_block(id) || _block_id_to_block.contains(id);
}

/**
 * Only return true *if* the transaction has not expired or been invalidated. If this
 * method is called with a VERY old transaction we will return false, they should
 * query things by blocks if they are that old.
 */
bool database::is_known_transaction( const transaction_id_type& id )const
{
   const auto& trx_idx = get_index_type<transaction_index>().indices().get<by_trx_id>();
   return trx_idx.find( id ) != trx_idx.end();
}

block_id_type  database::get_block_id_for_num( uint32_t block_num )const
{
   try
   {
      return _block_id_to_block.fetch_block_id( block_num );
   }
   FC_CAPTURE_AND_RETHROW( (block_num) )
}

optional<signed_block> database::fetch_block_by_id( const block_id_type& id )const
{
   auto b = _fork_db.fetch_block( id );
   if( !b )
      return _block_id_to_block.fetch_optional(id);
   return b->data;
}

optional<signed_block> database::fetch_block_by_number( uint32_t num )const
{
   auto results = _fork_db.fetch_block_by_number(num);
   if( results.size() == 1 )
      return results[0]->data;
   else
      return _block_id_to_block.fetch_by_number(num);
   return optional<signed_block>();
}

const signed_transaction& database::get_recent_transaction(const transaction_id_type& trx_id) const
{
   auto& index = get_index_type<transaction_index>().indices().get<by_trx_id>();
   auto itr = index.find(trx_id);
   FC_ASSERT(itr != index.end());
   return itr->trx;
}

std::vector<block_id_type> database::get_block_ids_on_fork(block_id_type head_of_fork) const
{
   pair<fork_database::branch_type, fork_database::branch_type> branches = _fork_db.fetch_branch_from(head_block_id(), head_of_fork);
   if( branches.first.back()->previous_id() != branches.second.back()->previous_id() )
   {
      edump( (head_of_fork)
             (head_block_id())
             (branches.first.size())
             (branches.second.size()) );
      assert( false );
   }
   std::vector<block_id_type> result;
   for( const item_ptr& fork_block : branches.second )
      result.emplace_back(fork_block->id);
   result.emplace_back(branches.first.back()->previous_id());
   return result;
}

chain_id_type database::get_chain_id() const
{
   return MUSE_CHAIN_ID;
}

const fc::sha256& database::get_genesis_json_hash()const
{
   return genesis_json_hash;
}

const account_object& database::get_account( const string& name )const
{
   const auto& accounts_by_name = get_index_type<account_index>().indices().get<by_name>();
   auto itr = accounts_by_name.find(name);
   FC_ASSERT(itr != accounts_by_name.end(),
             "Unable to find account '${acct}'. Did you forget to add a record for it?",
             ("acct", name));
   return *itr;
}

const escrow_object& database::get_escrow( const string& name, uint32_t escrow_id )const {
   const auto& escrow_idx = get_index_type<escrow_index>().indices().get<by_from_id>();
   auto itr = escrow_idx.find( boost::make_tuple(name,escrow_id) );
   FC_ASSERT( itr != escrow_idx.end() );
   return *itr;
}

const limit_order_object* database::find_limit_order( const string& name, uint32_t orderid )const
{
   const auto& orders_by_account = get_index_type<limit_order_index>().indices().get<by_account>();
   auto itr = orders_by_account.find(boost::make_tuple(name,orderid));
   if( itr == orders_by_account.end() ) return nullptr;
   return &*itr;
}

const limit_order_object& database::get_limit_order( const string& name, uint32_t orderid )const
{
   const auto& orders_by_account = get_index_type<limit_order_index>().indices().get<by_account>();
   auto itr = orders_by_account.find(boost::make_tuple(name,orderid));
   FC_ASSERT(itr != orders_by_account.end(),
             "Unable to find order '${acct}/${id}'.",
             ("acct", name)("id",orderid));
   return *itr;
}

const witness_object& database::get_witness( const string& name ) const
{
   const auto& witnesses_by_name = get_index_type< witness_index >().indices().get< by_name >();
   auto itr = witnesses_by_name.find( name );
   FC_ASSERT( itr != witnesses_by_name.end(),
              "Unable to find witness account '${wit}'. Did you forget to add a record for it?",
              ( "wit", name ) );
   return *itr;
}

const witness_object* database::find_witness( const string& name ) const
{
   const auto& witnesses_by_name = get_index_type< witness_index >().indices().get< by_name >();
   auto itr = witnesses_by_name.find( name );
   if( itr == witnesses_by_name.end() ) return nullptr;
   return &*itr;
}

const streaming_platform_object& database::get_streaming_platform( const string& name ) const
{
   const auto& streaming_platform_by_name = get_index_type< streaming_platform_index >().indices().get< by_name >();
   auto itr = streaming_platform_by_name.find( name );
   FC_ASSERT( itr != streaming_platform_by_name.end(),
              "Unable to find streaming_platform account '${wit}'. Did you forget to add a record for it?",
              ( "wit", name ) );
   return *itr;
}

const streaming_platform_object* database::find_streaming_platform( const string& name ) const
{
   const auto& streaming_platform_by_name = get_index_type< streaming_platform_index >().indices().get< by_name >();
   auto itr = streaming_platform_by_name.find( name );
   if( itr == streaming_platform_by_name.end() ) return nullptr;
   return &*itr;
}

const content_object& database::get_content( const string& url )const
{
   try{
      const auto& by_url_idx = get_index_type< content_index >().indices().get< by_url >();
      auto itr = by_url_idx.find(url);
      FC_ASSERT( itr != by_url_idx.end() );
      return *itr;
   }
   FC_CAPTURE_AND_RETHROW((url))
}

void database::pay_fee( const account_object& account, asset fee )
{
   FC_ASSERT( fee.amount >= 0 ); /// NOTE if this fails then validate() on some operation is probably wrong
   if( fee.amount == 0 )
      return;

   adjust_balance( account, -fee );
   adjust_supply( -fee );
}

void database::update_account_bandwidth( const account_object& a, uint32_t trx_size ) {

   const auto& props = get_dynamic_global_properties();
   if( props.total_vesting_shares.amount > 0 )
   {
      const auto now = head_block_time();
      modify( a, [&props,&now,this,trx_size]( account_object& acnt )
      {
         auto delta_time = (now - acnt.last_bandwidth_update).to_seconds();
         uint64_t N = trx_size * MUSE_BANDWIDTH_PRECISION;
         acnt.lifetime_bandwidth += N;
         if( delta_time >= MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS )
            acnt.average_bandwidth = N;
         else if( has_hardfork(MUSE_HARDFORK_0_4) )
         {
            auto old_weight = acnt.average_bandwidth;
            if( delta_time > 0 )
               old_weight = old_weight * (MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS - delta_time) / (MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS);
            acnt.average_bandwidth = old_weight + N;
         }
         else
         {  // TODO: remove after HF
            auto old_weight = acnt.average_bandwidth * (MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS - delta_time);
            auto new_weight = delta_time * N;
            acnt.average_bandwidth =  (old_weight + new_weight) / (MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS);
         }

         fc::uint128 account_vshares( get_effective_vesting_shares(acnt, VESTS_SYMBOL).amount.value );
         if( account_vshares == 0 )
            account_vshares = fc::uint128( 1 );

         fc::uint128 total_vshares( props.total_vesting_shares.amount.value );
         fc::uint128 account_average_bandwidth( acnt.average_bandwidth );
         fc::uint128 max_virtual_bandwidth( props.max_virtual_bandwidth );

         // account_vshares / total_vshares  > account_average_bandwidth / max_virtual_bandwidth
         FC_ASSERT( (account_vshares * max_virtual_bandwidth) > (account_average_bandwidth * total_vshares),
                    "account exceeded maximum allowed bandwidth per vesting share account_vshares: ${account_vshares} account_average_bandwidth: ${account_average_bandwidth} max_virtual_bandwidth: ${max_virtual_bandwidth} total_vesting_shares: ${total_vesting_shares}",
                    ("account_vshares",account_vshares)
                    ("account_average_bandwidth",account_average_bandwidth)
                    ("max_virtual_bandwidth",max_virtual_bandwidth)
                    ("total_vshares",total_vshares) );
         acnt.last_bandwidth_update = now;
      } );
   }
}


void database::update_account_market_bandwidth( const account_object& a, uint32_t trx_size )
{
   const auto& props = get_dynamic_global_properties();
   if( props.total_vesting_shares.amount > 0 )
   {
      const auto now = head_block_time();
      modify( a, [&props,&now,this,trx_size]( account_object& acnt )
      {
         auto delta_time = (now - acnt.last_market_bandwidth_update).to_seconds();
         uint64_t N = trx_size * MUSE_BANDWIDTH_PRECISION;
         if( delta_time >= MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS )
            acnt.average_market_bandwidth = N;
         else
         {
            auto old_weight = acnt.average_market_bandwidth * (MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS - delta_time);
            auto new_weight = delta_time * N;
            acnt.average_market_bandwidth =  (old_weight + new_weight) / (MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS);
         }

         fc::uint128 account_vshares( get_effective_vesting_shares(acnt, VESTS_SYMBOL).amount.value );
         FC_ASSERT( account_vshares > 0, "only accounts with a positive vesting balance may transact" );

         fc::uint128 total_vshares( props.total_vesting_shares.amount.value );

         fc::uint128 account_average_bandwidth( acnt.average_market_bandwidth );
         fc::uint128 max_virtual_bandwidth( props.max_virtual_bandwidth / 10 ); /// only 10% of bandwidth can be market

         // account_vshares / total_vshares  > account_average_bandwidth / max_virtual_bandwidth
         FC_ASSERT( (account_vshares * max_virtual_bandwidth) > (account_average_bandwidth * total_vshares),
                    "account exceeded maximum allowed bandwidth per vesting share account_vshares: ${account_vshares} account_average_bandwidth: ${account_average_bandwidth} max_virtual_bandwidth: ${max_virtual_bandwidth} total_vesting_shares: ${total_vesting_shares}",
                    ("account_vshares",account_vshares)
                    ("account_average_bandwidth",account_average_bandwidth)
                    ("max_virtual_bandwidth",max_virtual_bandwidth)
                    ("total_vshares",total_vshares) );
         acnt.last_market_bandwidth_update = now;
      } );
   }
}

asset database::get_effective_vesting_shares( const account_object& account, asset_id_type vested_symbol )const
{
   if( vested_symbol == VESTS_SYMBOL )
      return account.vesting_shares - account.delegated_vesting_shares + account.received_vesting_shares
             - account.redelegated_vesting_shares + account.rereceived_vesting_shares;
   FC_ASSERT( false, "Invalid symbol" );
}

uint32_t database::witness_participation_rate()const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   return uint64_t(MUSE_100_PERCENT) * dpo.recent_slots_filled.popcount() / 128;
}

void database::add_checkpoints( const flat_map<uint32_t,block_id_type>& checkpts )
{
   for( const auto& i : checkpts )
      _checkpoints[i.first] = i.second;
}

bool database::before_last_checkpoint()const
{
   return (_checkpoints.size() > 0) && (_checkpoints.rbegin()->first >= head_block_num());
}

/**
 * Push block "may fail" in which case every partial change is unwound.  After
 * push block is successful the block is appended to the chain database on disk.
 *
 * @return true if we switched forks as a result of this push.
 */
bool database::push_block(const signed_block& new_block, uint32_t skip)
{
   bool result;
   detail::with_skip_flags( *this, skip, [&]()
   {
      detail::without_pending_transactions( *this, std::move(_pending_tx),
      [&]()
      {
         try
         {
            result = _push_block(new_block);
         }
         FC_CAPTURE_AND_RETHROW( (new_block) )
      });
   });
   return result;
}

bool database::_push_block(const signed_block& new_block)
{
   uint32_t skip = get_node_properties().skip_flags;
   if( !(skip&skip_fork_db) )
   {
      shared_ptr<fork_item> new_head = _fork_db.push_block(new_block);
      //If the head block from the longest chain does not build off of the current head, we need to switch forks.
      if( new_head->data.previous != head_block_id() )
      {
         //If the newly pushed block is the same height as head, we get head back in new_head
         //Only switch forks if new_head is actually higher than head
         if( new_head->data.block_num() > head_block_num() )
         {
            wlog( "Switching to fork: ${id}", ("id",new_head->data.id()) );
            auto branches = _fork_db.fetch_branch_from(new_head->data.id(), head_block_id());

            // pop blocks until we hit the forked block
            while( head_block_id() != branches.second.back()->data.previous )
            {
               ilog( "popping block #${n} ${id}", ("n",head_block_num())("id",head_block_id()) );
               pop_block();
            }

            // push all blocks on the new fork
            for( auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr )
            {
                ilog( "pushing block from fork #${n} ${id}", ("n",(*ritr)->data.block_num())("id",(*ritr)->id) );
                optional<fc::exception> except;
                try
                {
                   undo_database::session session = _undo_db.start_undo_session();
                   apply_block( (*ritr)->data, skip );
                   _block_id_to_block.store( (*ritr)->id, (*ritr)->data );
                   session.commit();
                }
                catch ( const fc::exception& e ) { except = e; }
                if( except )
                {
                   wlog( "exception thrown while switching forks ${e}", ("e",except->to_detail_string() ) );
                   // remove the rest of branches.first from the fork_db, those blocks are invalid
                   while( ritr != branches.first.rend() )
                   {
                      ilog( "removing block from fork_db #${n} ${id}", ("n",(*ritr)->data.block_num())("id",(*ritr)->id) );
                      _fork_db.remove( (*ritr)->id );
                      ++ritr;
                   }
                   _fork_db.set_head( branches.second.front() );

                   // pop all blocks from the bad fork
                   while( head_block_id() != branches.second.back()->data.previous )
                   {
                      ilog( "popping block #${n} ${id}", ("n",head_block_num())("id",head_block_id()) );
                      pop_block();
                   }

                   ilog( "Switching back to fork: ${id}", ("id",branches.second.front()->data.id()) );
                   // restore all blocks from the good fork
                   for( auto ritr2 = branches.second.rbegin(); ritr2 != branches.second.rend(); ++ritr2 )
                   {
                      ilog( "pushing block #${n} ${id}", ("n",(*ritr2)->data.block_num())("id",(*ritr2)->id) );
                      auto session = _undo_db.start_undo_session();
                      apply_block( (*ritr2)->data, skip );
                      _block_id_to_block.store( (*ritr2)->id, (*ritr2)->data );
                      session.commit();
                   }
                   throw *except;
                }
            }
            return true;
         }
         else
            return false;
      }
      _opened = true;
   }

   try
   {
      auto session = _undo_db.start_undo_session();
      apply_block(new_block, skip);
      _block_id_to_block.store(new_block.id(), new_block);
      session.commit();
   }
   catch( const fc::exception& e )
   {
      elog("Failed to push new block:\n${e}", ("e", e.to_detail_string()));
      _fork_db.remove(new_block.id());
      throw;
   }

   return false;
}

/**
 * Attempts to push the transaction into the pending queue
 *
 * When called to push a locally generated transaction, set the skip_block_size_check bit on the skip argument. This
 * will allow the transaction to be pushed even if it causes the pending block size to exceed the maximum block size.
 * Although the transaction will probably not propagate further now, as the peers are likely to have their pending
 * queues full as well, it will be kept in the queue to be propagated later when a new block flushes out the pending
 * queues.
 */
void database::push_transaction( const signed_transaction& trx, uint32_t skip )
{
   try
   {
      try
      {
         FC_ASSERT( fc::raw::pack_size(trx) <= (get_dynamic_global_properties().maximum_block_size - 256) );
         set_producing( true );
         detail::with_skip_flags( *this, skip, [&]() { _push_transaction( trx ); } );
         set_producing(false);
      }
      catch( ... )
      {
         set_producing(false);
         throw;
      }
   }
   FC_CAPTURE_AND_RETHROW( (trx) )
}

void database::_push_transaction( const signed_transaction& trx )
{
   // If this is the first transaction pushed after applying a block, start a new undo session.
   // This allows us to quickly rewind to the clean state of the head block, in case a new block arrives.
   if( !_pending_tx_session.valid() )
      _pending_tx_session = _undo_db.start_undo_session();

   // Create a temporary undo session as a child of _pending_tx_session.
   // The temporary session will be discarded by the destructor if
   // _apply_transaction fails.  If we make it to merge(), we
   // apply the changes.

   auto temp_session = _undo_db.start_undo_session();
   _apply_transaction( trx );
   _pending_tx.push_back( trx );

   notify_changed_objects();
   // The transaction applied successfully. Merge its changes into the pending block session.
   temp_session.merge();

   // notify anyone listening to pending transactions
   on_pending_transaction( trx );
}

signed_block database::generate_block(
   fc::time_point_sec when,
   const string& witness_owner,
   const fc::ecc::private_key& block_signing_private_key,
   uint32_t skip /* = 0 */
   )
{
   signed_block result;
   detail::with_skip_flags( *this, skip, [&]()
   {
      try
      {
         result = _generate_block( when, witness_owner, block_signing_private_key );
      }
      FC_CAPTURE_AND_RETHROW( (witness_owner) )
   } );
   return result;
}

class soft_fork_checker {
public:
   using result_type = void;

   template<typename Op>
   void operator()( const Op& op )const {}

   void operator()( const muse::chain::proposal_create_operation& v )const {
      for( const op_wrapper &op : v.proposed_ops )
         op.op.visit( *this );
   }

   void operator()( const asset_create_operation& op )const {
      FC_ASSERT( "federation" == op.issuer || "federation.asset" == op.issuer,
                 "Only 'federation' and 'federation.asset' accounts can create assets!" );
   }
};

static void check_soft_fork( const transaction& tx ) {
   static soft_fork_checker vtor;

   for( const auto& op : tx.operations )
      op.visit( vtor );
}

signed_block database::_generate_block(
   fc::time_point_sec when,
   const string& witness_owner,
   const fc::ecc::private_key& block_signing_private_key
   )
{
   uint32_t skip = get_node_properties().skip_flags;
   uint32_t slot_num = get_slot_at_time( when );
   FC_ASSERT( slot_num > 0 );
   string scheduled_witness = get_scheduled_witness( slot_num );
   FC_ASSERT( scheduled_witness == witness_owner );

   const auto& witness_obj = get_witness( witness_owner );

   if( !(skip & skip_witness_signature) )
      FC_ASSERT( witness_obj.signing_key == block_signing_private_key.get_public_key() );

   signed_block pending_block;

   pending_block.previous = head_block_id();
   pending_block.timestamp = when;
   pending_block.witness = witness_owner;
   const auto& witness = get_witness( witness_owner );

   if( witness.running_version != MUSE_BLOCKCHAIN_VERSION )
      pending_block.extensions.insert( block_header_extensions( MUSE_BLOCKCHAIN_VERSION ) );

   const auto& hfp = hardfork_property_id_type()( *this );

   if( hfp.current_hardfork_version < MUSE_BLOCKCHAIN_HARDFORK_VERSION // Binary is newer hardfork than has been applied
      && ( witness.hardfork_version_vote != _hardfork_versions[ hfp.last_hardfork + 1 ] || witness.hardfork_time_vote != _hardfork_times[ hfp.last_hardfork + 1 ] ) ) // Witness vote does not match binary configuration
   {
      // Make vote match binary configuration
      pending_block.extensions.insert( block_header_extensions( hardfork_version_vote( _hardfork_versions[ hfp.last_hardfork + 1 ], _hardfork_times[ hfp.last_hardfork + 1 ] ) ) );
   }
   else if( hfp.current_hardfork_version == MUSE_BLOCKCHAIN_HARDFORK_VERSION // Binary does not know of a new hardfork
      && witness.hardfork_version_vote > MUSE_BLOCKCHAIN_HARDFORK_VERSION ) // Voting for hardfork in the future, that we do not know of...
   {
      // Make vote match binary configuration. This is vote to not apply the new hardfork.
      pending_block.extensions.insert( block_header_extensions( hardfork_version_vote( _hardfork_versions[ hfp.last_hardfork ], _hardfork_times[ hfp.last_hardfork ] ) ) );
   }
   // The 4 is for the max size of the transaction vector length
   size_t total_block_size = fc::raw::pack_size( pending_block ) + 4;
   auto maximum_block_size = get_dynamic_global_properties().maximum_block_size;

   //
   // The following code throws away existing pending_tx_session and
   // rebuilds it by re-applying pending transactions.
   //
   // This rebuild is necessary because pending transactions' validity
   // and semantics may have changed since they were received, because
   // time-based semantics are evaluated based on the current block
   // time.  These changes can only be reflected in the database when
   // the value of the "when" variable is known, which means we need to
   // re-apply pending transactions in this method.
   //
   _pending_tx_session.reset();
   _pending_tx_session = _undo_db.start_undo_session();

   uint64_t postponed_tx_count = 0;
   // pop pending state (reset to head block state)
   for( const signed_transaction& tx : _pending_tx )
   {
      // Only include transactions that have not expired yet for currently generating block,
      // this should clear problem transactions and allow block production to continue

      if( tx.expiration < when )
         continue;

      uint64_t new_total_size = total_block_size + fc::raw::pack_size( tx );

      // postpone transaction if it would make block too big
      if( new_total_size >= maximum_block_size )
      {
         postponed_tx_count++;
         continue;
      }

      try
      {
         if( !has_hardfork( MUSE_HARDFORK_0_6 ) ) check_soft_fork( tx );

         auto temp_session = _undo_db.start_undo_session();
         _apply_transaction( tx );
         temp_session.merge();

         total_block_size += fc::raw::pack_size( tx );
         pending_block.transactions.push_back( tx );
      }
      catch ( const fc::exception& e )
      {
         // Do nothing, transaction will not be re-applied
         wlog( "Transaction was not processed while generating block due to ${e}", ("e", e) );
         wlog( "The transaction was ${t}", ("t", tx) );
      }
   }
   if( postponed_tx_count > 0 )
   {
      wlog( "Postponed ${n} transactions due to block size limit", ("n", postponed_tx_count) );
   }

   _pending_tx_session.reset();

   // We have temporarily broken the invariant that
   // _pending_tx_session is the result of applying _pending_tx, as
   // _pending_tx now consists of the set of postponed transactions.
   // However, the push_block() call below will re-create the
   // _pending_tx_session.

   pending_block.transaction_merkle_root = pending_block.calculate_merkle_root();

   if( !(skip & skip_witness_signature) )
      pending_block.sign( block_signing_private_key );

   // TODO:  Move this to _push_block() so session is restored.
   if( !(skip & skip_block_size_check) )
   {
      FC_ASSERT( fc::raw::pack_size(pending_block) <= MUSE_MAX_BLOCK_SIZE );
   }

   push_block( pending_block, skip );

   return pending_block;
}

/**
 * Removes the most recent block from the database and
 * undoes any changes it made.
 */
void database::pop_block()
{
   try
   {
      _pending_tx_session.reset();
      auto head_id = head_block_id();

      /// save the head block so we can recover its transactions
      optional<signed_block> head_block = fetch_block_by_id( head_id );
      MUSE_ASSERT( head_block.valid(), pop_empty_chain, "there are no blocks to pop" );

      _fork_db.pop_block();
      pop_undo();

      _popped_tx.insert( _popped_tx.begin(), head_block->transactions.begin(), head_block->transactions.end() );

   }
   FC_CAPTURE_AND_RETHROW()
}

void database::clear_pending()
{
   try
   {
      assert( (_pending_tx.size() == 0) || _pending_tx_session.valid() );
      _pending_tx.clear();
      _pending_tx_session.reset();
   }
   FC_CAPTURE_AND_RETHROW()
}

void database::push_applied_operation( const operation& op )
{
   operation_object obj;
   obj.trx_id       = _current_trx_id;
   obj.block        = _current_block_num;
   obj.trx_in_block = _current_trx_in_block;
   obj.op_in_trx    = _current_op_in_trx;
   obj.virtual_op   = _current_virtual_op++;
   obj.op           = op;

   pre_apply_operation( obj );
}


void database::notify_post_apply_operation( const operation& op )
{
   operation_object obj;
   obj.trx_id       = _current_trx_id;
   obj.block        = _current_block_num;
   obj.trx_in_block = _current_trx_in_block;
   obj.op_in_trx    = _current_op_in_trx;
   obj.virtual_op   = _current_virtual_op;
   obj.op           = op;
   post_apply_operation( obj );
}

string database::get_scheduled_witness( uint32_t slot_num )const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   const witness_schedule_object& wso = witness_schedule_id_type()(*this);
   uint64_t current_aslot = dpo.current_aslot + slot_num;
   return wso.current_shuffled_witnesses[ current_aslot % wso.current_shuffled_witnesses.size() ];
}

fc::time_point_sec database::get_slot_time(uint32_t slot_num)const
{
   if( slot_num == 0 )
      return fc::time_point_sec();

   auto interval = MUSE_BLOCK_INTERVAL;
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();

   if( head_block_num() == 0 )
   {
      // n.b. first block is at genesis_time plus one block interval
      fc::time_point_sec genesis_time = dpo.time;
      return genesis_time + slot_num * interval;
   }

   int64_t head_block_abs_slot = head_block_time().sec_since_epoch() / interval;
   fc::time_point_sec head_slot_time( head_block_abs_slot * interval );

   // "slot 0" is head_slot_time
   // "slot 1" is head_slot_time,
   //   plus maint interval if head block is a maint block
   //   plus block interval if head block is not a maint block
   return head_slot_time + (slot_num * interval);
}

uint32_t database::get_slot_at_time(fc::time_point_sec when)const
{
   fc::time_point_sec first_slot_time = get_slot_time( 1 );
   if( when < first_slot_time )
      return 0;
   return (when - first_slot_time).to_seconds() / MUSE_BLOCK_INTERVAL + 1;
}

/**
 *  Converts MUSE into mbd and adds it to to_account while reducing the MUSE supply
 *  by MUSE and increasing the mbd supply by the specified amount.
 */
asset database::create_mbd(const account_object &to_account, asset muse)
{
   try
   {
      if( muse.amount == 0 )
         return asset(0, MBD_SYMBOL);

      const auto& median_price = get_feed_history().actual_median_history;
      if( !median_price.is_null() )
      {
         auto mbd = muse * median_price;

         adjust_balance( to_account, mbd );
         adjust_supply( -muse );
         adjust_supply( mbd );
         return mbd;
      }
      else
      {
         adjust_balance( to_account, muse );
         return muse;
      }
   }
   FC_CAPTURE_LOG_AND_RETHROW( (to_account.name)(muse) )
}

/**
 * @param to_account - the account to receive the new vesting shares
 * @param MUSE - MUSE to be converted to vesting shares
 */
asset database::create_vesting( const account_object& to_account, asset muse )
{
   try
   {
      const auto& cprops = get_dynamic_global_properties();

      /**
       *  The ratio of total_vesting_shares / total_vesting_fund_muse should not
       *  change as the result of the user adding funds
       *
       *  V / C  = (V+Vn) / (C+Cn)
       *
       *  Simplifies to Vn = (V * Cn ) / C
       *
       *  If Cn equals o.amount, then we must solve for Vn to know how many new vesting shares
       *  the user should receive.
       *
       *  128 bit math is requred due to multiplying of 64 bit numbers. This is done in asset and price.
       */
      asset new_vesting = muse * cprops.get_vesting_share_price();

      modify( to_account, [&]( account_object& to )
      {
         to.vesting_shares += new_vesting;
      } );

      const streaming_platform_object* sp = find_streaming_platform( to_account.name );
      modify( cprops, [muse,new_vesting,sp]( dynamic_global_property_object& props )
      {
         props.total_vesting_fund_muse += muse;
         props.total_vesting_shares += new_vesting;
         if( sp )
             props.total_vested_by_platforms += new_vesting.amount;
      } );

      adjust_proxied_witness_votes( to_account, new_vesting.amount );
      recursive_recalculate_score( to_account, new_vesting.amount);
      return new_vesting;
   }
   FC_CAPTURE_AND_RETHROW( (to_account.name)(muse) )
}

bool database::is_voted_streaming_platform(string streaming_platform)const
{
   //this is simple version to check if given account has been voted in as streaming_platform AT THE MOMENT of check...
   //not sute yet if this can cause any issues, e.g. race condition. Test carefully. 
   int count =0;
   const auto& spidx = get_index_type<streaming_platform_index>().indices().get<by_vote_name>();
   for ( auto itr = spidx.begin(); 
         itr != spidx.end() && count < MUSE_MAX_VOTED_STREAMING_PLATFORMS;
         ++itr, ++count )
   {
      if (itr->owner.compare(streaming_platform) == 0)
         return true;
   }
   return false;
}

bool database::is_streaming_platform(string streaming_platform)const
{
   int count =0;
   const auto& spidx = get_index_type<streaming_platform_index>().indices().get<by_vote_name>();
   for ( auto&& itr = spidx.begin();
         itr != spidx.end() && count < MUSE_MAX_VOTED_STREAMING_PLATFORMS;
         ++itr )
   {
      if (itr->owner.compare(streaming_platform) == 0)
         return true;
      count++;
   }
   return false;
}

vector<string> database::get_voted_streaming_platforms()
{
   int count =0;
   vector<string> voted_sp;
   const auto& spidx = get_index_type<streaming_platform_index>().indices().get<by_vote_name>();
   for ( auto itr = spidx.begin();
         itr != spidx.end() && count < MUSE_MAX_VOTED_STREAMING_PLATFORMS;
          ++itr, ++count ) 
   {
      if (itr!=spidx.end())
         voted_sp.push_back(itr->owner);
   }

   return voted_sp;
}

void database::update_witness_schedule4()
{
   vector<string> active_witnesses;

   /// Add the highest voted witnesses
   flat_set<witness_id_type> selected_voted;
   selected_voted.reserve( MUSE_MAX_VOTED_WITNESSES );
   const auto& widx         = get_index_type<witness_index>().indices().get<by_vote_name>();
   for( auto itr = widx.begin();
        itr != widx.end() && selected_voted.size() <  MUSE_MAX_VOTED_WITNESSES;
        ++itr )
   {
      if( has_hardfork( MUSE_HARDFORK_0_3 ) && (itr->signing_key == public_key_type()) )
         continue; // skip witnesses without a valid block signing key
      selected_voted.insert(itr->get_id());
      active_witnesses.push_back(itr->owner);
   }

   /// Add the running witnesses in the lead
   const witness_schedule_object& wso = witness_schedule_id_type()(*this);
   fc::uint128 new_virtual_time = wso.current_virtual_time;
   const auto& schedule_idx = get_index_type<witness_index>().indices().get<by_schedule_time>();
   auto sitr = schedule_idx.begin();
   vector<decltype(sitr)> processed_witnesses;
   for( auto witness_count = selected_voted.size();
        sitr != schedule_idx.end() && witness_count < MUSE_MAX_MINERS;
        ++sitr )
   {
      new_virtual_time = sitr->virtual_scheduled_time; /// everyone advances to at least this time
      processed_witnesses.push_back(sitr);
      if( has_hardfork( MUSE_HARDFORK_0_3 ) && sitr->signing_key == public_key_type() )
         continue; // skip witnesses without a valid block signing key
      if( selected_voted.find(sitr->get_id()) == selected_voted.end() )
      {
         active_witnesses.push_back(sitr->owner);
         ++witness_count;
      }
   }

   /// Update virtual schedule of processed witnesses
   bool reset_virtual_time = false;
   for( auto itr = processed_witnesses.begin(); itr != processed_witnesses.end(); ++itr )
   {
      auto new_virtual_scheduled_time = new_virtual_time + VIRTUAL_SCHEDULE_LAP_LENGTH2 / ((*itr)->votes.value+1);
      if( new_virtual_scheduled_time < new_virtual_time )
      {
         reset_virtual_time = true; /// overflow
         break;
      }
      modify( *(*itr), [&]( witness_object& wo )
      {
         wo.virtual_position        = fc::uint128();
         wo.virtual_last_update     = new_virtual_time;
         wo.virtual_scheduled_time  = new_virtual_scheduled_time;
      } );
   }
   if( reset_virtual_time )
   {
      new_virtual_time = fc::uint128();
      reset_virtual_schedule_time();
   }

   FC_ASSERT( active_witnesses.size() <= MUSE_MAX_MINERS, "number of active witnesses does not equal MUSE_MAX_MINERS",
                                       ("active_witnesses.size()",active_witnesses.size()) ("MUSE_MAX_MINERS",MUSE_MAX_MINERS) );

   auto majority_version = wso.majority_version;

   flat_map< version, uint32_t, std::greater< version > > witness_versions;
   flat_map< std::tuple< hardfork_version, time_point_sec >, uint32_t > hardfork_version_votes;

   for( uint32_t i = 0; i < wso.current_shuffled_witnesses.size(); i++ )
   {
      auto witness = get_witness( wso.current_shuffled_witnesses[ i ] );
      if( witness_versions.find( witness.running_version ) == witness_versions.end() )
         witness_versions[ witness.running_version ] = 1;
      else
         witness_versions[ witness.running_version ] += 1;

      auto version_vote = std::make_tuple( witness.hardfork_version_vote, witness.hardfork_time_vote );
      if( hardfork_version_votes.find( version_vote ) == hardfork_version_votes.end() )
         hardfork_version_votes[ version_vote ] = 1;
      else
         hardfork_version_votes[ version_vote ] += 1;
   }

   int witnesses_on_version = 0;
   auto ver_itr = witness_versions.begin();

   // The map should be sorted highest version to smallest, so we iterate until we hit the majority of witnesses on at least this version
   while( ver_itr != witness_versions.end() )
   {
      witnesses_on_version += ver_itr->second;

      if( witnesses_on_version >= MUSE_HARDFORK_REQUIRED_WITNESSES )
      {
         majority_version = ver_itr->first;
         break;
      }

      ++ver_itr;
   }

   auto hf_itr = hardfork_version_votes.begin();

   while( hf_itr != hardfork_version_votes.end() )
   {
      if( hf_itr->second >= MUSE_HARDFORK_REQUIRED_WITNESSES )
      {
         modify( hardfork_property_id_type()( *this ), [&]( hardfork_property_object& hpo )
         {
            hpo.next_hardfork = std::get<0>( hf_itr->first );
            hpo.next_hardfork_time = std::get<1>( hf_itr->first );
         } );

         break;
      }

      ++hf_itr;
   }

   // We no longer have a majority
   if( hf_itr == hardfork_version_votes.end() )
   {
      modify( hardfork_property_id_type()( *this ), [&]( hardfork_property_object& hpo )
      {
         hpo.next_hardfork = hpo.current_hardfork_version;
      });
   }

   modify( wso, [&]( witness_schedule_object& _wso )
   {
      _wso.current_shuffled_witnesses = active_witnesses;

      /// shuffle current shuffled witnesses
      auto now_hi = uint64_t(head_block_time().sec_since_epoch()) << 32;
      for( uint32_t i = 0; i < _wso.current_shuffled_witnesses.size(); ++i )
      {
         /// High performance random generator
         /// http://xorshift.di.unimi.it/
         uint64_t k = now_hi + uint64_t(i)*2685821657736338717ULL;
         k ^= (k >> 12);
         k ^= (k << 25);
         k ^= (k >> 27);
         k *= 2685821657736338717ULL;

         uint32_t jmax = _wso.current_shuffled_witnesses.size() - i;
         uint32_t j = i + k%jmax;
         std::swap( _wso.current_shuffled_witnesses[i],
                    _wso.current_shuffled_witnesses[j] );
      }

      _wso.current_virtual_time = new_virtual_time;
      _wso.next_shuffle_block_num = head_block_num() + _wso.current_shuffled_witnesses.size();
      _wso.majority_version = majority_version;
   } );

   update_median_witness_props();
}


/**
 *
 *  See @ref witness_object::virtual_last_update
 */
void database::update_witness_schedule()
{
   if( (head_block_num() % MUSE_MAX_MINERS) == 0 )
      update_witness_schedule4();
}

void database::update_median_witness_props()
{
   const witness_schedule_object& wso = witness_schedule_id_type()(*this);

   /// fetch all witness objects
   vector<const witness_object*> active; active.reserve( wso.current_shuffled_witnesses.size() );
   for( const auto& wname : wso.current_shuffled_witnesses )
   {
      active.push_back(&get_witness(wname));
   }

   /// sort them by account_creation_fee
   std::sort( active.begin(), active.end(), [&]( const witness_object* a, const witness_object* b )
   {
      return a->props.account_creation_fee.amount < b->props.account_creation_fee.amount;
   } );

   modify( wso, [&]( witness_schedule_object& _wso )
   {
     _wso.median_props.account_creation_fee = active[active.size()/2]->props.account_creation_fee;
   } );

   /// sort them by streaming_platform_update_fee
   std::sort( active.begin(), active.end(), [&]( const witness_object* a, const witness_object* b )
   {
        return a->props.streaming_platform_update_fee.amount < b->props.streaming_platform_update_fee.amount;
   } );

   modify( wso, [&]( witness_schedule_object& _wso )
   {
        _wso.median_props.streaming_platform_update_fee = active[active.size()/2]->props.streaming_platform_update_fee;
   } );

   /// sort them by maximum_block_size
   std::sort( active.begin(), active.end(), [&]( const witness_object* a, const witness_object* b )
   {
      return a->props.maximum_block_size < b->props.maximum_block_size;
   } );

   modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& p )
   {
         p.maximum_block_size = active[active.size()/2]->props.maximum_block_size;
   } );


   /// sort them by mbd_interest_rate
   std::sort( active.begin(), active.end(), [&]( const witness_object* a, const witness_object* b )
   {
      return a->props.mbd_interest_rate < b->props.mbd_interest_rate;
   } );

   modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& p )
   {
         p.mbd_interest_rate = active[active.size()/2]->props.mbd_interest_rate;
   } );
}

void database::adjust_proxied_witness_votes( const account_object& a,
                                   const std::array< share_type, MUSE_MAX_PROXY_RECURSION_DEPTH+1 >& delta,
                                   int depth )
{
   if( a.proxy != MUSE_PROXY_TO_SELF_ACCOUNT )
   {
      /// nested proxies are not supported, vote will not propagate
      if( depth >= MUSE_MAX_PROXY_RECURSION_DEPTH )
         return;

      const auto& proxy = get_account( a.proxy );

      modify( proxy, [&]( account_object& a )
      {
         for( int i = MUSE_MAX_PROXY_RECURSION_DEPTH - depth - 1; i >= 0; --i )
         {
            a.proxied_vsf_votes[i+depth] += delta[i];
         }
      } );

      adjust_proxied_witness_votes( proxy, delta, depth + 1 );
   }
   else
   {
      share_type total_delta = 0;
      for( int i = MUSE_MAX_PROXY_RECURSION_DEPTH - depth; i >= 0; --i )
         total_delta += delta[i];
      adjust_witness_votes( a, total_delta );
      adjust_streaming_platform_votes( a, total_delta );
   }
}

void database::adjust_proxied_witness_votes( const account_object& a, share_type delta, int depth )
{
   if( a.proxy != MUSE_PROXY_TO_SELF_ACCOUNT )
   {
      /// nested proxies are not supported, vote will not propagate
      if( depth >= MUSE_MAX_PROXY_RECURSION_DEPTH )
         return;

      const auto& proxy = get_account( a.proxy );

      modify( proxy, [&]( account_object& a )
      {
         a.proxied_vsf_votes[depth] += delta;
      } );

      adjust_proxied_witness_votes( proxy, delta, depth + 1 );
   }
   else
   {
      adjust_witness_votes( a, delta );
      adjust_streaming_platform_votes( a, delta );
   }
}

void database::adjust_witness_votes( const account_object& a, share_type delta )
{
   const auto& vidx = get_index_type<witness_vote_index>().indices().get<by_account_witness>();
   auto itr = vidx.lower_bound( boost::make_tuple( a.get_id(), witness_id_type() ) );
   while( itr != vidx.end() && itr->account == a.get_id() )
   {
      adjust_witness_vote( itr->witness(*this), delta );
      ++itr;
   }
}

void database::adjust_witness_vote( const witness_object& witness, share_type delta )
{
   const witness_schedule_object& wso = witness_schedule_id_type()(*this);
   modify( witness, [&]( witness_object& w )
   {
      auto delta_pos = w.votes.value * (wso.current_virtual_time - w.virtual_last_update);
      w.virtual_position += delta_pos;

      w.virtual_last_update = wso.current_virtual_time;
      w.votes += delta;
      FC_ASSERT( w.votes <= get_dynamic_global_properties().total_vesting_shares.amount, "", ("w.votes", w.votes)("props",get_dynamic_global_properties().total_vesting_shares) );

      w.virtual_scheduled_time = w.virtual_last_update + (VIRTUAL_SCHEDULE_LAP_LENGTH2 - w.virtual_position)/(w.votes.value+1);

      /** witnesses with a low number of votes could overflow the time field and end up with a scheduled time in the past */
      if( w.virtual_scheduled_time < wso.current_virtual_time )
         w.virtual_scheduled_time = fc::uint128::max_value();
   } );
}

void database::adjust_streaming_platform_votes( const account_object& a, share_type delta )
{
   const auto& vidx = get_index_type<streaming_platform_vote_index>().indices().get<by_account_streaming_platform>();
   auto itr = vidx.lower_bound( boost::make_tuple( a.get_id(), streaming_platform_id_type() ) );
   while( itr != vidx.end() && itr->account == a.get_id() )
   {
      adjust_streaming_platform_vote( itr->streaming_platform(*this), delta );
      ++itr;
   }
}

void database::adjust_streaming_platform_vote( const streaming_platform_object& sp, share_type delta )
{
   modify( sp, [&](streaming_platform_object& spo)
   {
      spo.votes+=delta;
      FC_ASSERT( spo.votes <=  get_dynamic_global_properties().total_vesting_shares.amount, "", ("sp.votes", spo.votes)("props",get_dynamic_global_properties().total_vesting_shares) );
   } );
}


void database::clear_witness_votes( const account_object& a )
{
   const auto& vidx = get_index_type<witness_vote_index>().indices().get<by_account_witness>();
   auto itr = vidx.lower_bound( boost::make_tuple( a.get_id(), witness_id_type() ) );
   while( itr != vidx.end() && itr->account == a.get_id() )
   {
      const auto& current = *itr;
      ++itr;
      remove(current);
   }

   modify( a, [&](account_object& acc )
   {
      acc.witnesses_voted_for = 0;
   });
}

void database::clear_streaming_platform_votes( const account_object& a )
{
   const auto& vidx = get_index_type<streaming_platform_vote_index>().indices().get<by_account_streaming_platform>();
   auto itr = vidx.lower_bound( boost::make_tuple( a.get_id(), streaming_platform_id_type() ) );
   while( itr != vidx.end() && itr->account == a.get_id() )
   {
      const auto& current = *itr;
      ++itr;
      remove(current);
   }

   modify( a, [&](account_object& acc )
   {
      acc.streaming_platforms_voted_for = 0;
   });
}


void database::update_owner_authority( const account_object& account, const authority& owner_authority )
{
   const auto now = head_block_time();
   create< owner_authority_history_object >( [&account,now]( owner_authority_history_object& hist )
   {
      hist.account = account.name;
      hist.previous_owner_authority = account.owner;
      hist.last_valid_time = now;
   });

   modify( account, [&owner_authority,now]( account_object& a )
   {
      a.owner = owner_authority;
      a.last_owner_update = now;
   });
}

void database::process_vesting_withdrawals()
{
   const auto& widx = get_index_type< account_index >().indices().get< by_next_vesting_withdrawal >();
   const auto& didx = get_index_type< withdraw_vesting_route_index >().indices().get< by_withdraw_route >();
   auto current = widx.begin();

   const auto& cprops = get_dynamic_global_properties();

   while( current != widx.end() && current->next_vesting_withdrawal <= head_block_time() )
   {
      const auto& from_account = *current; ++current;

      /**
      *  Let T = total tokens in vesting fund
      *  Let V = total vesting shares
      *  Let v = total vesting shares being cashed out
      *
      *  The user may withdraw  vT / V tokens
      */
      share_type to_withdraw;
      if ( from_account.to_withdraw - from_account.withdrawn < from_account.vesting_withdraw_rate.amount )
         to_withdraw = std::min( from_account.vesting_shares.amount, from_account.to_withdraw % from_account.vesting_withdraw_rate.amount ).value;
      else
         to_withdraw = std::min( from_account.vesting_shares.amount, from_account.vesting_withdraw_rate.amount ).value;

      share_type vests_deposited_as_muse = 0;
      share_type vests_deposited_as_vests = 0;

      // Do two passes, the first for vests, the second for muse. Try to maintain as much accuracy for vests as possible.
      for( auto itr = didx.upper_bound( boost::make_tuple( from_account.id, account_id_type() ) );
           itr != didx.end() && itr->from_account == from_account.id;
           ++itr )
      {
         if( itr->auto_vest )
         {
            share_type to_deposit = ( ( fc::uint128_t ( to_withdraw.value ) * itr->percent ) / MUSE_100_PERCENT ).to_uint64();
            vests_deposited_as_vests += to_deposit;

            if( to_deposit > 0 )
            {
               const auto& to_account = itr->to_account( *this );

               modify( to_account, [&]( account_object& a )
               {
                  a.vesting_shares.amount += to_deposit;
               });

               adjust_proxied_witness_votes( to_account, to_deposit );
               recursive_recalculate_score( to_account, to_deposit);
               push_applied_operation( fill_vesting_withdraw_operation( from_account.name, to_account.name, asset( to_deposit, VESTS_SYMBOL ), asset( to_deposit, VESTS_SYMBOL ) ) );
            }
         }
      }

      for( auto itr = didx.upper_bound( boost::make_tuple( from_account.id, account_id_type() ) );
           itr != didx.end() && itr->from_account == from_account.id;
           ++itr )
      {
         if( !itr->auto_vest )
         {
            const auto& to_account = itr->to_account( *this );

            share_type to_deposit = ( ( fc::uint128_t ( to_withdraw.value ) * itr->percent ) / MUSE_100_PERCENT ).to_uint64();
            vests_deposited_as_muse += to_deposit;
            auto converted_muse = asset( to_deposit, VESTS_SYMBOL ) * cprops.get_vesting_share_price();

            if( to_deposit > 0 )
            {
               modify( to_account, [&]( account_object& a )
               {
                  a.balance += converted_muse;
               });

               modify( cprops, [&]( dynamic_global_property_object& o )
               {
                  o.total_vesting_fund_muse -= converted_muse;
                  o.total_vesting_shares.amount -= to_deposit;
               });

               push_applied_operation( fill_vesting_withdraw_operation( from_account.name, to_account.name, asset( to_deposit, VESTS_SYMBOL), converted_muse ) );
            }
         }
      }

      share_type to_convert = to_withdraw - vests_deposited_as_muse - vests_deposited_as_vests;
      FC_ASSERT( to_convert >= 0, "Deposited more vests than were supposed to be withdrawn" );

      auto converted_muse = asset( to_convert, VESTS_SYMBOL ) * cprops.get_vesting_share_price();

      modify( from_account, [&]( account_object& a )
      {
         a.vesting_shares.amount -= to_withdraw;
         a.balance += converted_muse;
         a.withdrawn += to_withdraw;

         if( a.withdrawn >= a.to_withdraw || a.vesting_shares.amount == 0 )
         {
            a.vesting_withdraw_rate.amount = 0;
            a.next_vesting_withdrawal = fc::time_point_sec::maximum();
         }
         else
         {
            a.next_vesting_withdrawal += fc::seconds( MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS );
         }
      });

      modify( cprops, [&]( dynamic_global_property_object& o )
      {
         o.total_vesting_fund_muse -= converted_muse;
         o.total_vesting_shares.amount -= to_convert;
      });

      if( to_withdraw > 0 ) {
         adjust_proxied_witness_votes(from_account, -to_withdraw);
         recursive_recalculate_score(from_account, -to_withdraw);
      }

      push_applied_operation( fill_vesting_withdraw_operation( from_account.name, from_account.name, asset( to_convert, VESTS_SYMBOL ), converted_muse ) );
   }
}

struct sp_helper
{
   const streaming_platform_object* sp;
   const account_object* sp_acct;
   flat_map<account_id_type, uint32_t> account_listening_times;
   flat_map<uint64_t, uint32_t> user_listening_times;
   uint64_t anon_listening_time = 0;

   share_type get_vesting_stake()const
   {
      return sp_acct->vesting_shares.amount
             + sp_acct->received_vesting_shares.amount - sp_acct->delegated_vesting_shares.amount;
   }
};

static asset calculate_report_reward( const database& db, const dynamic_global_property_object& dgpo,
                                      const asset& total_payout, const uint32_t play_time,
                                      const sp_helper& platform, const uint64_t total_listening_time )
{
   share_type stake = platform.get_vesting_stake();
   if( stake.value == 0 || total_payout.amount.value == 0 )
      return asset( 0, total_payout.asset_id );
   FC_ASSERT( total_payout.amount.value > 0 );

   dlog("process content cashout ", ("total_listening_time", total_listening_time));

   fc::uint128_t pay_reserve = total_payout.amount.value;
   pay_reserve *= play_time;
   if( !db.has_hardfork( MUSE_HARDFORK_0_2 ) )
      pay_reserve = pay_reserve / dgpo.active_users;
   else if( !db.has_hardfork( MUSE_HARDFORK_0_5 ) )
      pay_reserve = pay_reserve * std::min( total_listening_time, uint64_t(3600) ) / dgpo.full_users_time;
   else
   {
      pay_reserve = pay_reserve * stake.value / dgpo.total_vested_by_platforms.value;
      pay_reserve = pay_reserve * std::min( total_listening_time, uint64_t(3600) )
                                / platform.sp->full_users_time;
   }
   pay_reserve = pay_reserve / total_listening_time;

   return asset( pay_reserve.to_uint64(), total_payout.asset_id );
}

template<typename K>
static void adjust_listening_times( flat_map<K, uint32_t>& listening_times, K consumer, uint32_t play_time )
{
   auto listened = listening_times.find( consumer );
   if( listened == listening_times.end() )
      listening_times[consumer] = play_time;
   else
      listened->second += play_time;
};

static void adjust_delta( const uint32_t time_before, const uint32_t time_after,
                          uint32_t& active_users, uint32_t& full_time_users,
                          uint32_t& total_listening_time, uint32_t& full_users_time )
{
   total_listening_time += time_before - time_after;
   if( time_after < 3600 )
   {
      if( time_after == 0 )
         ++active_users;
      if( time_before >= 3600 )
      {
         ++full_time_users;
         full_users_time += 3600 - time_after;
      }
      else
         full_users_time += time_before - time_after;
   }
}

static void adjust_statistics( database& db, const dynamic_global_property_object& dgpo,
                               const flat_map<streaming_platform_id_type, sp_helper> platforms )
{
   const bool adjust_consumer_total = db.has_hardfork( MUSE_HARDFORK_0_2 );

   const auto& sp_user_idx = db.get_index_type< streaming_platform_user_index >().indices().get< by_consumer >();
   uint32_t global_active_users_delta = 0;
   uint32_t global_full_time_users_delta = 0;
   uint32_t global_total_listening_time_delta = 0;
   uint32_t global_full_users_time_delta = 0;
   for( const auto& sph : platforms )
   {
      uint32_t platform_active_users_delta = 0;
      uint32_t platform_full_time_users_delta = 0;
      uint32_t platform_total_listening_time_delta = 0;
      uint32_t platform_full_users_time_delta = 0;
      // count normal users
      for ( const auto& listened : sph.second.account_listening_times )
      {
         const account_object& consumer = db.get<account_object>( listened.first );
         auto global_time_before = consumer.total_listening_time;
         if( !adjust_consumer_total ) global_time_before += listened.second;
         auto ptb = consumer.total_time_by_platform.find( sph.first );
         FC_ASSERT( ptb != consumer.total_time_by_platform.end() );
         const uint32_t platform_time_before = ptb->second;
         db.modify( consumer, [&listened,adjust_consumer_total,&sph] ( account_object& a ) {
            if( adjust_consumer_total )
               a.total_listening_time -= listened.second;
            auto entry = a.total_time_by_platform.find( sph.first );
            FC_ASSERT( entry != a.total_time_by_platform.end() );
            entry->second -= listened.second;
            if( entry->second == 0 )
               a.total_time_by_platform.erase( sph.first );
         });

         adjust_delta( global_time_before, consumer.total_listening_time,
                       global_active_users_delta, global_full_time_users_delta,
                       global_total_listening_time_delta, global_full_users_time_delta );
         ptb = consumer.total_time_by_platform.find( sph.first );
         adjust_delta( platform_time_before, ptb == consumer.total_time_by_platform.end() ? 0 : ptb->second,
                       platform_active_users_delta, platform_full_time_users_delta,
                       platform_total_listening_time_delta, platform_full_users_time_delta );
      }
      // count pseudonymous users
      for ( const auto& listened : sph.second.user_listening_times )
      {
         const auto& itr = sp_user_idx.find( boost::make_tuple( sph.first, listened.first ) );
         const streaming_platform_user_object& consumer = *itr;
         auto global_time_before = consumer.total_listening_time;
         if( consumer.total_listening_time == listened.second )
            db.remove( consumer );
         else
            db.modify( consumer, [&listened,adjust_consumer_total,&sph] ( streaming_platform_user_object& a ) {
               a.total_listening_time -= listened.second;
            });

         adjust_delta( global_time_before, global_time_before - listened.second,
                       global_active_users_delta, global_full_time_users_delta,
                       global_total_listening_time_delta, global_full_users_time_delta );
         adjust_delta( global_time_before, global_time_before - listened.second,
                       platform_active_users_delta, platform_full_time_users_delta,
                       platform_total_listening_time_delta, platform_full_users_time_delta );
      }
      // count anon user
      if( sph.second.anon_listening_time > 0 )
      {
         adjust_delta( sph.second.sp->total_anon_listening_time,
                       sph.second.sp->total_anon_listening_time - sph.second.anon_listening_time,
                       global_active_users_delta, global_full_time_users_delta,
                       global_total_listening_time_delta, global_full_users_time_delta );
         adjust_delta( sph.second.sp->total_anon_listening_time,
                       sph.second.sp->total_anon_listening_time - sph.second.anon_listening_time,
                       platform_active_users_delta, platform_full_time_users_delta,
                       platform_total_listening_time_delta, platform_full_users_time_delta );
      }
      if( platform_total_listening_time_delta > 0 || platform_full_users_time_delta > 0
            || platform_full_time_users_delta > 0 || platform_active_users_delta > 0 )
         db.modify( *sph.second.sp, [platform_total_listening_time_delta,platform_full_users_time_delta,
                                     platform_full_time_users_delta,platform_active_users_delta,&sph]
                                    ( streaming_platform_object& o ) {
            o.active_users -= platform_active_users_delta;
            o.full_time_users -= platform_full_time_users_delta;
            o.total_listening_time -= platform_total_listening_time_delta;
            o.full_users_time -= platform_full_users_time_delta;
            o.total_anon_listening_time -= sph.second.anon_listening_time;
         });
   } // for platforms

   if( global_total_listening_time_delta > 0 || global_full_users_time_delta > 0
         || global_full_time_users_delta > 0 || global_active_users_delta > 0 )
      db.modify( dgpo, [global_total_listening_time_delta,global_full_users_time_delta,
                        global_full_time_users_delta,global_active_users_delta]
                       ( dynamic_global_property_object& o ) {
         o.active_users -= global_active_users_delta;
         o.full_time_users -= global_full_time_users_delta;
         o.total_listening_time -= global_total_listening_time_delta;
         o.full_users_time -= global_full_users_time_delta;
      });
}

asset database::process_content_cashout( const asset& content_reward )
{ try {
   auto now = head_block_time();
   auto cashing_time = now - fc::seconds(60*24*60);
   asset paid(0);
   
   asset total_payout = has_hardfork( MUSE_HARDFORK_0_2 ) ? content_reward : get_content_reward();

   const auto& sp_user_idx = get_index_type< streaming_platform_user_index >().indices().get< by_consumer >();
   const auto& ridx = get_index_type<report_index>().indices().get<by_created>();
   const auto& dgpo = get_dynamic_global_properties();
   auto itr = ridx.begin();
   flat_map<streaming_platform_id_type, sp_helper> platforms;
   while ( itr != ridx.end() && itr->created <= cashing_time )
   {
      const streaming_platform_id_type spinner_id = itr->spinning_platform.valid()
                                                         ? *itr->spinning_platform : itr->streaming_platform;
      auto sp = platforms.find( spinner_id );
      if( sp == platforms.end() )
      {
         sp_helper tmp;
         tmp.sp = &get<streaming_platform_object>( spinner_id );
         tmp.sp_acct = &get_account( tmp.sp->owner );
         platforms[spinner_id] = std::move( tmp );
         sp = platforms.find( spinner_id );
      }

      const account_object* consumer_account = nullptr;
      const streaming_platform_user_object* consumer_sp_user = nullptr;
      uint64_t total_listening_time = 0;
      if( itr->consumer.valid() )
      {
         consumer_account = &get<account_object>( *itr->consumer );
         if( !has_hardfork( MUSE_HARDFORK_0_5 ) )
            total_listening_time = consumer_account->total_listening_time;
         else
         {
            auto time_entry = consumer_account->total_time_by_platform.find( spinner_id );
            FC_ASSERT( time_entry != consumer_account->total_time_by_platform.end() );
            total_listening_time = time_entry->second;
         }
      }
      else if( itr->sp_user_id.valid() )
      {
         const auto& spu = sp_user_idx.find( boost::make_tuple( spinner_id, *itr->sp_user_id ) );
         if( spu != sp_user_idx.end() ) // should always be true
         {
            consumer_sp_user = &(*spu);
            total_listening_time = consumer_sp_user->total_listening_time;
         }
      }
      else
         total_listening_time = sp->second.sp->total_anon_listening_time;
      auto report_reward = calculate_report_reward( *this, dgpo, total_payout, itr->play_time, sp->second,
                                                    total_listening_time );
      const content_object& content = get<content_object>( itr->content );
      auto content_payment = pay_to_content( content, report_reward, itr->streaming_platform );
      paid += content_payment;
      if( has_hardfork( MUSE_HARDFORK_0_5 ) )
      {
         auto platform_reward = report_reward - content_payment;
         asset reporter_reward;
         if( itr->spinning_platform.valid() && itr->reward_pct.valid() )
         {
            reporter_reward = asset( platform_reward.amount * *itr->reward_pct / MUSE_100_PERCENT,
                                   platform_reward.asset_id );
            if( platform_reward.amount > reporter_reward.amount )
               pay_to_platform( *itr->spinning_platform, platform_reward - reporter_reward, content.url );
         }
         else
            reporter_reward = platform_reward;

         if( reporter_reward.amount > 0 )
            pay_to_platform( itr->streaming_platform, reporter_reward, content.url );

         paid += platform_reward;
      }
      else // before hf_0_5 platform_reward is paid out in pay_to_content()
         if( !has_hardfork( MUSE_HARDFORK_0_2 ) )
            modify( *consumer_account, [&itr]( account_object& a ) {
               a.total_listening_time -= itr->play_time;
            });

      if( consumer_account != nullptr )
         adjust_listening_times( sp->second.account_listening_times, account_id_type(consumer_account->id),
                                 itr->play_time );
      else if( consumer_sp_user != nullptr )
         adjust_listening_times( sp->second.user_listening_times, consumer_sp_user->sp_user_id, itr->play_time );
      else
         sp->second.anon_listening_time += itr->play_time;

      remove(*itr);
      itr = ridx.begin();
   }

   adjust_statistics( *this, dgpo, platforms );

   return paid;
} FC_LOG_AND_RETHROW() }

void database::pay_to_content_master(const content_object &co, const asset& payout)
{try{
   if ( co.distributions_master.size() == 0)
   {
      modify(co, [&payout]( content_object& c ){
         c.accumulated_balance_master += payout;
      });
   }
   else
   {
      asset to_pay = payout;
      if (has_hardfork( MUSE_HARDFORK_0_2 ))
         to_pay += co.accumulated_balance_master;
      asset total_paid = asset( 0, to_pay.asset_id );
      for ( const auto& di : co.distributions_master )
      {
         asset author_reward = to_pay;
         author_reward.amount = author_reward.amount * di.bp / 10000;
         total_paid += author_reward;

         auto mbd_muse     = author_reward;
         auto vesting_muse = author_reward - mbd_muse;

         const auto& author = get_account( di.payee );
         auto vest_created = create_vesting( author, vesting_muse );
         auto mbd_created = create_mbd( author, mbd_muse );

         push_applied_operation( content_reward_operation( di.payee, co.url, mbd_created, vest_created ) );
      }
      if( total_paid > to_pay )
         elog( "Paid out too much for content master ${co}: ${paid} > ${to_pay}",
               ("co",co)("paid",total_paid)("to_pay",to_pay) );
      to_pay -= total_paid;
      if( !has_hardfork( MUSE_HARDFORK_0_2 ) )
      {
         if( to_pay.amount != 0 )
            modify(co, [&to_pay]( content_object& c ){
               c.accumulated_balance_master += to_pay;
            });
      }
      else if( co.accumulated_balance_master != to_pay )
         modify(co, [&to_pay]( content_object& c ){
            c.accumulated_balance_master = to_pay;
         });
   }
}FC_LOG_AND_RETHROW() }

void database::pay_to_content_comp(const content_object &co, const asset& payout)
{try{
   if ( co.distributions_comp.size() == 0)
   {
      modify(co, [&payout]( content_object& c ){
         c.accumulated_balance_comp += payout;
      });
   }
   else
   {
      asset to_pay = payout;
      if (has_hardfork( MUSE_HARDFORK_0_2 ))
         to_pay += co.accumulated_balance_comp;
      asset total_paid = asset( 0, to_pay.asset_id );
      for ( const auto& di : co.distributions_comp )
      {
         asset author_reward = to_pay;
         author_reward.amount = author_reward.amount * di.bp / 10000;
         total_paid += author_reward;

         auto mbd_muse     = author_reward;
         auto vesting_muse = author_reward - mbd_muse;

         const auto& author = get_account( di.payee );
         auto vest_created = create_vesting( author, vesting_muse );
         auto mbd_created = create_mbd( author, mbd_muse );

         push_applied_operation( content_reward_operation( di.payee, co.url, mbd_created, vest_created ) );
      }
      if( total_paid > to_pay )
         elog( "Paid out too much for content composer ${co}: ${paid} > ${to_pay}",
               ("co",co)("paid",total_paid)("to_pay",to_pay) );
      to_pay -= total_paid;
      if( !has_hardfork( MUSE_HARDFORK_0_2 ) )
      {
         if( to_pay.amount != 0 )
            modify(co, [&to_pay]( content_object& c ){
               c.accumulated_balance_comp += to_pay;
            });
      }
      else if( co.accumulated_balance_comp != to_pay )
         modify(co, [&to_pay]( content_object& c ){
            c.accumulated_balance_comp = to_pay;
         });
   }
}FC_LOG_AND_RETHROW() }

void database::pay_to_platform( streaming_platform_id_type platform, const asset& payout, const string& url )
{try{
   const streaming_platform_object& pl = get<streaming_platform_object>( platform );
   const auto& owner = get_account(pl.owner);
   auto mbd_muse     = asset(0, MUSE_SYMBOL);
   auto vesting_muse = payout - mbd_muse;
   auto vest_created = create_vesting( owner, vesting_muse );
   auto mbd_created = create_mbd( owner, mbd_muse );
   push_applied_operation(playing_reward_operation(pl.owner, url, mbd_created, vest_created ));
}FC_LOG_AND_RETHROW() }

asset database::pay_to_content(const content_object& content, asset payout, streaming_platform_id_type platform)
{try{
   asset paid (0);
   if( !has_hardfork(MUSE_HARDFORK_0_2) )
      payout = payout - payout * MUSE_CURATE_APR_PERCENT_RESERVE / 100; // former curation reward
   asset platform_reward = payout;
   platform_reward.amount = platform_reward.amount * content.playing_reward / MUSE_100_PERCENT;

   payout.amount -= platform_reward.amount;
   asset comp_reward = payout;
   comp_reward.amount = comp_reward.amount * content.publishers_share / MUSE_100_PERCENT;
   asset master_reward = payout - comp_reward;

   pay_to_content_master(content, master_reward);
   paid += master_reward;
   pay_to_content_comp(content, comp_reward);
   paid += comp_reward;
   if( !has_hardfork(MUSE_HARDFORK_0_5) )
   {
      pay_to_platform( platform, platform_reward, content.url );
      paid += platform_reward;
   }

   modify<content_object>( content, []( content_object& c ) {
      --c.times_played_24;
   });

   return paid;
}FC_LOG_AND_RETHROW() }



/**
 *  Overall the network has an inflation rate of 9.5% of virtual muse per year
 *  74.25% of inflation is directed to content
 *  0.75% of inflation is directed to curators
 *  15% of inflation is directed to liquidity providers
 *  10% of inflation is directed to block producers
 *
 *  This method pays out vesting and reward shares every block, and liquidity shares once per day.
 *  This method does not pay out witnesses.
 */
void database::process_funds( const asset& content_reward, const asset& witness_pay, const asset& vesting_reward )
{
   const auto& props = get_dynamic_global_properties();

   modify( props, [&content_reward, &witness_pay, &vesting_reward]( dynamic_global_property_object& p )
   {
       p.total_vesting_fund_muse += vesting_reward;
       p.total_reward_fund_muse  += content_reward ;
       p.current_supply += content_reward + witness_pay + vesting_reward;
       p.virtual_supply += content_reward + witness_pay + vesting_reward;
   } );
}

void database::adjust_funds(const asset& content_reward, const asset& paid_to_content)
{
   const auto initial_content_allocation = has_hardfork( MUSE_HARDFORK_0_2 ) ? content_reward : get_content_reward();
   const asset delta = initial_content_allocation - paid_to_content;
   const asset true_delta = content_reward - paid_to_content;
   const auto& props = get_dynamic_global_properties();
   modify( props, [&delta, &true_delta]( dynamic_global_property_object& p )
   {
        p.total_reward_fund_muse  -= delta;
        p.current_supply -= delta;
        p.virtual_supply -= delta;
        p.supply_delta += delta - true_delta;
   } );
}

asset database::get_content_reward()const
{
   const auto& props = get_dynamic_global_properties();

   static_assert( MUSE_BLOCK_INTERVAL == 3, "this code assumes a 3-second time interval" );
   const auto amount = has_hardfork(MUSE_HARDFORK_0_2) ? calc_percent_reward_per_day_0_2< MUSE_CONTENT_APR_PERCENT_0_2 >( props.virtual_supply.amount )
                                                       : calc_percent_reward_per_day< MUSE_CONTENT_APR_PERCENT >( props.virtual_supply.amount );
   return std::max( asset( amount, MUSE_SYMBOL ), MUSE_MIN_CONTENT_REWARD );
}

asset database::get_vesting_reward()const
{
   const auto& props = get_dynamic_global_properties();
   static_assert( MUSE_BLOCK_INTERVAL == 3, "this code assumes a 3-second time interval" );
   const auto amount = has_hardfork(MUSE_HARDFORK_0_2) ? calc_percent_reward_per_block_0_2< MUSE_VESTING_ARP_PERCENT_0_2 >( props.virtual_supply.amount )
                                                       : calc_percent_reward_per_block< MUSE_VESTING_ARP_PERCENT >( props.virtual_supply.amount );
   return asset( amount, MUSE_SYMBOL );
}

asset database::get_producer_reward()
{
   const auto& props = get_dynamic_global_properties();
   static_assert( MUSE_BLOCK_INTERVAL == 3, "this code assumes a 3-second time interval" );

   const auto amount = has_hardfork(MUSE_HARDFORK_0_2) ? calc_percent_reward_per_block_0_2< MUSE_PRODUCER_APR_PERCENT_0_2 >( props.virtual_supply.amount)
                                                       : calc_percent_reward_per_block< MUSE_PRODUCER_APR_PERCENT >( props.virtual_supply.amount );
   const auto pay = std::max( asset( amount, MUSE_SYMBOL ), MUSE_MIN_PRODUCER_REWARD );
   const auto& witness_account = get_account( props.current_witness );

   /// pay witness in vesting shares
   if( props.head_block_number >= MUSE_START_MINER_VOTING_BLOCK || (witness_account.vesting_shares.amount.value == 0) )
   {
      create_vesting( witness_account, pay );
   }
   else
   {
      modify( get_account( witness_account.name), [&pay]( account_object& a )
      {
         a.balance += pay;
      } );
   }

   return pay;
}

/**
 *  Iterates over all conversion requests with a conversion date before
 *  the head block time and then converts them to/from muse/mbd at the
 *  current median price feed history price times the premium
 */
void database::process_conversions()
{
   auto now = head_block_time();
   const auto& request_by_date = get_index_type<convert_index>().indices().get<by_conversion_date>();
   auto itr = request_by_date.begin();

   const auto& fhistory = get_feed_history();
   if( fhistory.effective_median_history.is_null() )
      return;

   asset net_mbd( 0, MBD_SYMBOL );
   asset net_muse( 0, MUSE_SYMBOL );

   while( itr != request_by_date.end() && itr->conversion_date <= now )
   {
      const auto& user = get_account( itr->owner );
      auto amount_to_issue = itr->amount * fhistory.effective_median_history;

      adjust_balance( user, amount_to_issue );

      net_mbd   += itr->amount;
      net_muse += amount_to_issue;

      push_applied_operation( fill_convert_request_operation ( user.name, itr->requestid, itr->amount, amount_to_issue ) );

      remove( *itr );
      itr = request_by_date.begin();
   }

   const auto& props = get_dynamic_global_properties();
   modify( props, [&]( dynamic_global_property_object& p )
   {
       p.current_supply += net_muse;
       p.current_mbd_supply -= net_mbd;
       p.virtual_supply += net_muse;
       p.virtual_supply -= net_mbd * get_feed_history().effective_median_history;
   } );
}

asset database::to_mbd( const asset& muse )const
{
   FC_ASSERT( muse.asset_id == MUSE_SYMBOL );
   const auto& feed_history = get_feed_history();
   if( feed_history.actual_median_history.is_null() )
      return asset( 0, MBD_SYMBOL );

   return muse * feed_history.actual_median_history;
}

asset database::to_muse(const asset &mbd)const
{
   FC_ASSERT( mbd.asset_id == MBD_SYMBOL );
   const auto& feed_history = get_feed_history();
   if( feed_history.effective_median_history.is_null() )
      return asset( 0, MUSE_SYMBOL );

   return mbd * feed_history.effective_median_history;
}

void database::account_recovery_processing()
{
   // Clear expired recovery requests
   const auto& rec_req_idx = get_index_type< account_recovery_request_index >().indices().get< by_expiration >();
   auto rec_req = rec_req_idx.begin();

   while( rec_req != rec_req_idx.end() && rec_req->expires <= head_block_time() )
   {
      remove( *rec_req );
      rec_req = rec_req_idx.begin();
   }

   // Clear invalid historical authorities
   const auto& hist_idx = get_index_type< owner_authority_history_index >().indices(); //by id
   auto hist = hist_idx.begin();

   while( hist != hist_idx.end() && time_point_sec( hist->last_valid_time + MUSE_OWNER_AUTH_RECOVERY_PERIOD ) < head_block_time() )
   {
      remove( *hist );
      hist = hist_idx.begin();
   }

   // Apply effective recovery_account changes
   const auto& change_req_idx = get_index_type< change_recovery_account_request_index >().indices().get< by_effective_date >();
   auto change_req = change_req_idx.begin();

   while( change_req != change_req_idx.end() && change_req->effective_on <= head_block_time() )
   {
      modify( get_account( change_req->account_to_recover ), [&]( account_object& a )
      {
         a.recovery_account = change_req->recovery_account;
      });

      remove( *change_req );
      change_req = change_req_idx.begin();
   }
}

const dynamic_global_property_object&database::get_dynamic_global_properties() const
{
   return get( dynamic_global_property_id_type() );
}

const node_property_object& database::get_node_properties() const
{
   return _node_property_object;
}

time_point_sec database::head_block_time()const
{
   return get( dynamic_global_property_id_type() ).time;
}

uint32_t database::head_block_num()const
{
   return get( dynamic_global_property_id_type() ).head_block_number;
}

block_id_type database::head_block_id()const
{
   return get( dynamic_global_property_id_type() ).head_block_id;
}

node_property_object& database::node_properties()
{
   return _node_property_object;
}

uint32_t database::last_non_undoable_block_num() const
{
   return head_block_num() - _undo_db.size();
}

void database::initialize_evaluators()
{
    _operation_evaluators.resize(255);

    register_evaluator<vote_evaluator>();
    register_evaluator<transfer_evaluator>();
    register_evaluator<transfer_to_vesting_evaluator>();
    register_evaluator<withdraw_vesting_evaluator>();
    register_evaluator<set_withdraw_vesting_route_evaluator>();
    register_evaluator<account_create_evaluator>();
    register_evaluator<account_create_with_delegation_evaluator>();
    register_evaluator<account_update_evaluator>();
    register_evaluator<witness_update_evaluator>();
    register_evaluator<streaming_platform_update_evaluator>();
    register_evaluator<request_stream_reporting_evaluator>();
    register_evaluator<cancel_stream_reporting_evaluator>();
    register_evaluator<account_witness_vote_evaluator>();
    register_evaluator<account_streaming_platform_vote_evaluator>();
    register_evaluator<account_witness_proxy_evaluator>();
    register_evaluator<custom_evaluator>();
    register_evaluator<custom_json_evaluator>();
    register_evaluator<report_over_production_evaluator>();
    register_evaluator<streaming_platform_report_evaluator>();
    register_evaluator<content_evaluator>();
    register_evaluator<content_update_evaluator>();
    register_evaluator<content_disable_evaluator>();
    register_evaluator<content_approve_evaluator>();

    register_evaluator<balance_claim_evaluator>();

    register_evaluator<proposal_create_evaluator>();
    register_evaluator<proposal_delete_evaluator>();
    register_evaluator<proposal_update_evaluator>();

    register_evaluator<feed_publish_evaluator>();
    register_evaluator<convert_evaluator>();
    register_evaluator<limit_order_create_evaluator>();
    register_evaluator<limit_order_create2_evaluator>();
    register_evaluator<limit_order_cancel_evaluator>();
    register_evaluator<challenge_authority_evaluator>();
    register_evaluator<prove_authority_evaluator>();
    register_evaluator<request_account_recovery_evaluator>();
    register_evaluator<recover_account_evaluator>();
    register_evaluator<change_recovery_account_evaluator>();
    register_evaluator<escrow_transfer_evaluator>();
    register_evaluator<escrow_dispute_evaluator>();
    register_evaluator<escrow_release_evaluator>();

    register_evaluator<asset_create_evaluator>();
    register_evaluator<asset_issue_evaluator>();
    register_evaluator<asset_reserve_evaluator>();
    register_evaluator<asset_update_evaluator>();

    register_evaluator<friendship_evaluator>();
    register_evaluator<unfriend_evaluator>();

    register_evaluator<delegate_vesting_shares_evaluator>();
}

void database::initialize_indexes()
{
   reset_indexes();
   _undo_db.set_max_size( MUSE_MIN_UNDO_HISTORY );

   //Protocol object indexes
   auto acnt_index = add_index< primary_index<account_index> >();
   acnt_index->add_secondary_index<account_member_index>();

   add_index< primary_index< streaming_platform_index > >();
   add_index< primary_index< stream_report_request_index > >();
   add_index< primary_index< report_index > >();
   add_index< primary_index< witness_index > >();
   add_index< primary_index< streaming_platform_vote_index > >();
   add_index< primary_index< streaming_platform_user_index > >();
   add_index< primary_index< witness_vote_index > >();
   add_index< primary_index< convert_index > >();
   add_index< primary_index< liquidity_reward_index > >();
   add_index< primary_index< limit_order_index > >();
   add_index< primary_index< escrow_index > >();
   auto cti = add_index< primary_index< content_index > >();
   cti->add_secondary_index<content_by_genre_index>();
   cti->add_secondary_index<content_by_category_index>();

   add_index< primary_index< content_approve_index> >();

   //Implementation object indexes
   add_index< primary_index< transaction_index                             > >();
   add_index< primary_index< simple_index< dynamic_global_property_object  > > >();
   add_index< primary_index< simple_index< feed_history_object             > > >();
   add_index< primary_index< flat_index<   block_summary_object            > > >();
   add_index< primary_index< simple_index< witness_schedule_object         > > >();
   add_index< primary_index< simple_index< hardfork_property_object        > > >();
   add_index< primary_index< withdraw_vesting_route_index                  > >();
   add_index< primary_index< owner_authority_history_index                 > >();
   add_index< primary_index< account_recovery_request_index                > >();
   add_index< primary_index< change_recovery_account_request_index         > >();
   add_index< primary_index< asset_index > >();
   add_index< primary_index< account_balance_index > >();

   auto prop_index = add_index< primary_index< proposal_index > >();
   prop_index->add_secondary_index<required_approval_index>();

   add_index< primary_index< content_vote_index > >();
   add_index< primary_index< balance_index > >();
   add_index< primary_index< vesting_delegation_index > >();
   add_index< primary_index< vesting_delegation_expiration_index > >();
}

void database::init_genesis( const genesis_state_type& initial_allocation )
{
   try
   {
      _undo_db.disable();
      class auth_inhibitor
      {
      public:
         explicit auth_inhibitor(database& db) : db(db), old_flags(db.node_properties().skip_flags)
         { db.node_properties().skip_flags |= skip_authority_check; }
         ~auth_inhibitor()
         { db.node_properties().skip_flags = old_flags; }
      private:
         database& db;
         uint32_t old_flags;
      };

      auth_inhibitor inhibitor(*this); FC_UNUSED(inhibitor);

      transaction_evaluation_state genesis_eval_state(this);

      flat_index<block_summary_object>& bsi = get_mutable_index_type< flat_index<block_summary_object> >();
      bsi.resize(0xffff+1);

      // Create blockchain accounts
      public_key_type      init_public_key(MUSE_INIT_PUBLIC_KEY);

      create<account_object>([this](account_object& a)
      {
         a.name = MUSE_MINER_ACCOUNT;
         a.owner.weight_threshold = 1;
         a.active.weight_threshold = 1;
      } );
      create<account_object>([this](account_object& a)
      {
         a.name = MUSE_NULL_ACCOUNT;
         a.owner.weight_threshold = 1;
         a.active.weight_threshold = 1;
      } );
      create<account_object>([this](account_object& a)
      {
         a.name = MUSE_TEMP_ACCOUNT;
         a.owner.weight_threshold = 0;
         a.active.weight_threshold = 0;
      } );

      for( int i = 0; i < MUSE_NUM_INIT_MINERS; ++i )
      {
         create<account_object>([&](account_object& a)
         {
            a.name = MUSE_INIT_MINER_NAME + ( i ? fc::to_string( i ) : std::string() );
            a.owner.weight_threshold = 1;
            a.owner.add_authority( init_public_key, 1 );
            a.active  = a.owner;
            a.basic = a.active;
            a.memo_key = init_public_key;
            a.balance  = asset( i ? 10 : initial_allocation.init_supply, MUSE_SYMBOL );
         } );

         create<witness_object>([&](witness_object& w )
         {
            w.owner        = MUSE_INIT_MINER_NAME + ( i ? fc::to_string(i) : std::string() );
            w.signing_key  = init_public_key;
         } );
      }

      auto gpo = create<dynamic_global_property_object>([&initial_allocation](dynamic_global_property_object& p)
      {
         p.current_witness = MUSE_INIT_MINER_NAME;
         p.time = MUSE_GENESIS_TIME;
         p.recent_slots_filled = fc::uint128::max_value();
         p.participation_count = 128;
         p.current_supply = asset( initial_allocation.init_supply + 10 * (MUSE_NUM_INIT_MINERS - 1), MUSE_SYMBOL );
         p.virtual_supply = p.current_supply;
         p.maximum_block_size = MUSE_MAX_BLOCK_SIZE;
      } );

      //Create core assets
      const asset_object& muse_asset = create<asset_object>( []( asset_object& a )
      {
         a.current_supply = 0;
         a.symbol_string = "MUSE";
         a.options.max_supply = MUSE_MAX_SHARE_SUPPLY;
         a.options.description = "MUSE Core asset";
      });
      create<asset_object>( []( asset_object& a )
      {
         a.current_supply = 0;
         a.symbol_string = "VEST";
         a.options.max_supply = MUSE_MAX_SHARE_SUPPLY;
         a.options.description = "MUSE Power asset";
      });
      create<asset_object>( []( asset_object& a )
      {
         a.current_supply = 0;
         a.symbol_string = "MBD";
         a.options.max_supply = MUSE_MAX_SHARE_SUPPLY;
         a.options.description = "MUSE backed dollars";
      });
 
      // Nothing to do
      create<feed_history_object>([&](feed_history_object& o) {});
      create<block_summary_object>([&](block_summary_object&) {});
      create<hardfork_property_object>([&](hardfork_property_object& hpo)
      {
         hpo.processed_hardforks.push_back( MUSE_GENESIS_TIME );
      } );

      // Create witness scheduler
      create<witness_schedule_object>([&]( witness_schedule_object& wso )
      {
         wso.current_shuffled_witnesses.push_back(MUSE_INIT_MINER_NAME);
      } );

      // Helper function to get account ID by name
      const auto& accounts_by_name = get_index_type<account_index>().indices().get<by_name>();
      auto get_account_id = [&accounts_by_name](const string& name) {
         auto itr = accounts_by_name.find(name);
         FC_ASSERT(itr != accounts_by_name.end(),
         "Unable to find account '${acct}'. Did you forget to add a record for it to initial_accounts?",
         ("acct", name));
         return itr->get_id();
      };

      ///////////////////////////////////////////////////////////
      //                 IMPORT                                //
      ///////////////////////////////////////////////////////////
      // import accounts
      for( const auto& account : initial_allocation.initial_accounts )
      {
         account_create_operation cop;
         cop.new_account_name = account.name;
         cop.creator = MUSE_TEMP_ACCOUNT;
         cop.owner = authority(1, account.owner_key, 1);
         if( account.active_key == public_key_type() )
         {
            cop.active = cop.owner;
            cop.memo_key = account.owner_key;
         }
         else
         {
            cop.active = authority(1, account.active_key, 1);
            cop.memo_key = account.active_key;
         }
         cop.basic = cop.active;

         apply_operation(genesis_eval_state, cop);
      }
      //assets
      for( const genesis_state_type::initial_asset_type& asset : initial_allocation.initial_assets )
      {
         create<asset_object>([&](asset_object& a) {
               a.symbol_string = asset.symbol;
               a.options.description = asset.description;
               string issuer_name = asset.issuer_name;
               a.issuer = get_account_id(issuer_name);
               a.options.max_supply = asset.max_supply;
               a.options.flags = disable_confidential;
               a.options.issuer_permissions = UIA_ASSET_ISSUER_PERMISSION_MASK;
         });
      }
      //initial balances
      for( const auto& handout : initial_allocation.initial_balances )
      {
         /*const auto asset_id = get_asset_id(handout.asset_symbol);
         account_object owner = get ( handout.owner );
         adjust_balance(owner, asset(handout.amount, asset_id));*/
         create<balance_object>([&](balance_object& b){
              b.owner = handout.owner;
              b.balance = asset( handout.amount, MUSE_SYMBOL );
         });

         modify( gpo, [&]( dynamic_global_property_object& p )
         {
            p.current_supply += asset( handout.amount, MUSE_SYMBOL );
            p.virtual_supply = p.current_supply;
         } );

         modify( muse_asset, [&]( asset_object& a ){
            a.current_supply += handout.amount;
         });
      }
      for( const genesis_state_type::initial_vesting_balance_type& vest : initial_allocation.initial_vesting_balances )
      {
         const asset new_vesting = asset( vest.amount, VESTS_SYMBOL );
         const auto& to_account = get(vest.owner);

         modify( to_account, [&new_vesting]( account_object& to )
         {
            to.vesting_shares += new_vesting;
         } );

         modify( gpo, [&new_vesting]( dynamic_global_property_object& props )
         {
            props.total_vesting_shares += new_vesting;
         } );
      }

      _undo_db.enable();
   }
   FC_CAPTURE_AND_RETHROW()
}


void database::validate_transaction( const signed_transaction& trx )
{
   auto session = _undo_db.start_undo_session();
   _apply_transaction( trx );
   FC_UNUSED(session); // will be rolled back by destructor
}

void database::notify_changed_objects()
{ try {
   if( _undo_db.enabled() )
   {
      const auto& head_undo = _undo_db.head();
      vector<object_id_type> changed_ids;  changed_ids.reserve(head_undo.old_values.size());
      for( const auto& item : head_undo.old_values ) changed_ids.push_back(item.first);
      for( const auto& item : head_undo.new_ids ) changed_ids.push_back(item);
      vector<const object*> removed;
      removed.reserve( head_undo.removed.size() );
      for( const auto& item : head_undo.removed )
      {
         changed_ids.push_back( item.first );
         removed.emplace_back( item.second.get() );
      }
      changed_objects(changed_ids);
   }
} FC_CAPTURE_AND_RETHROW() }

//////////////////// private methods ////////////////////

void database::apply_block( const signed_block& next_block, uint32_t skip )
{
   auto block_num = next_block.block_num();
   if( _checkpoints.size() && _checkpoints.rbegin()->second != block_id_type() )
   {
      auto itr = _checkpoints.find( block_num );
      if( itr != _checkpoints.end() )
         FC_ASSERT( next_block.id() == itr->second, "Block did not match checkpoint", ("checkpoint",*itr)("block_id",next_block.id()) );

      if( _checkpoints.rbegin()->first >= block_num )
         skip = skip_witness_signature
              | skip_transaction_signatures
              | skip_transaction_dupe_check
              | skip_fork_db
              | skip_block_size_check
              | skip_tapos_check
              | skip_authority_check
              | skip_merkle_check
              | skip_undo_history_check
              | skip_witness_schedule_check
              | skip_validate
              | skip_validate_invariants
              ;
   }

   detail::with_skip_flags( *this, skip, [this,&next_block]()
   {
      _apply_block( next_block );
   } );
}

void database::_apply_block( const signed_block& next_block )
{ try {
   uint32_t next_block_num = next_block.block_num();
   uint32_t skip = get_node_properties().skip_flags;

   FC_ASSERT( (skip & skip_merkle_check) || next_block.transaction_merkle_root == next_block.calculate_merkle_root(), "mysterious place...", ("next_block.transaction_merkle_root",next_block.transaction_merkle_root)("calc",next_block.calculate_merkle_root())("next_block",next_block)("id",next_block.id()) );

   const witness_object& signing_witness = validate_block_header(skip, next_block);

   _current_block_num    = next_block_num;
   _current_trx_in_block = 0;

   const auto& gprops = get_dynamic_global_properties();
   auto block_size = fc::raw::pack_size( next_block );
   FC_ASSERT( block_size <= gprops.maximum_block_size, "Block Size is too Big", ("next_block_num",next_block_num)("block_size", block_size)("max",gprops.maximum_block_size) );


   /// modify current witness so transaction evaluators can know who included the transaction,
   /// this is mostly for POW operations which must pay the current_witness
   modify( gprops, [&]( dynamic_global_property_object& dgp ){
      dgp.current_witness = next_block.witness;
   });

   /// parse witness version reporting
   process_header_extensions( next_block );

   FC_ASSERT( get_witness( next_block.witness ).running_version >= hardfork_property_id_type()( *this ).current_hardfork_version,
         "Block produced by witness that is not running current hardfork" );

   bool soft_fork = !has_hardfork( MUSE_HARDFORK_0_6 )
                    && next_block.timestamp >= fc::time_point::now() - fc::seconds(30);
   for( const auto& trx : next_block.transactions )
   {
      /* We do not need to push the undo state for each transaction
       * because they either all apply and are valid or the
       * entire block fails to apply.  We only need an "undo" state
       * for transactions when validating broadcast transactions or
       * when building a block.
       */
      if( soft_fork ) check_soft_fork( trx );
      apply_transaction( trx, skip );
      ++_current_trx_in_block;
   }

   update_global_dynamic_data(next_block);
   update_signing_witness(signing_witness, next_block);

   update_last_irreversible_block();

   create_block_summary(next_block);
   clear_expired_transactions();
   clear_expired_proposals();
   clear_expired_orders();
   clear_expired_delegations();
   update_witness_schedule();

   update_median_feed();
   update_virtual_supply();

   const auto content_reward = get_content_reward();
   const auto witness_pay = get_producer_reward();
   const auto vesting_reward = head_block_num() < MUSE_START_VESTING_BLOCK ? asset( 0, MUSE_SYMBOL )
                                                                           : get_vesting_reward();

   process_funds( content_reward, witness_pay, vesting_reward );
   process_conversions();
   asset paid_for_content = process_content_cashout( content_reward );
   adjust_funds( content_reward, paid_for_content );
   process_vesting_withdrawals();
   update_virtual_supply();

   account_recovery_processing();

   process_hardforks();

   // notify observers that the block has been applied
   applied_block( next_block ); //emit

   notify_changed_objects();
}
FC_LOG_AND_RETHROW() }

void database::process_header_extensions( const signed_block& next_block )
{
   auto itr = next_block.extensions.begin();

   while( itr != next_block.extensions.end() )
   {
      switch( itr->which() )
      {
         case 0: // void_t
            break;
         case 1: // version
         {
            auto reported_version = itr->get< version >();
            const auto& signing_witness = get_witness( next_block.witness );

            if( reported_version != signing_witness.running_version )
            {
               modify( signing_witness, [&]( witness_object& wo )
               {
                  wo.running_version = reported_version;
               });
            }
            break;
         }
         case 2: // hardfork_version vote
         {
            auto hfv = itr->get< hardfork_version_vote >();
            const auto& signing_witness = get_witness( next_block.witness );

            if( hfv.hf_version != signing_witness.hardfork_version_vote || hfv.hf_time != signing_witness.hardfork_time_vote )
               modify( signing_witness, [&]( witness_object& wo )
               {
                  wo.hardfork_version_vote = hfv.hf_version;
                  wo.hardfork_time_vote = hfv.hf_time;
               });

            break;
         }
         default:
            FC_ASSERT( false, "Unknown extension in block header" );
      }

      ++itr;
   }
}

const feed_history_object& database::get_feed_history()const {
   return feed_history_id_type()(*this);
}
const witness_schedule_object& database::get_witness_schedule_object()const {
   return witness_schedule_id_type()(*this);
}

void database::update_median_feed() {
try {
   if( (head_block_num() % MUSE_FEED_INTERVAL_BLOCKS) != 0 )
      return;

   auto now = head_block_time();
   const witness_schedule_object& wso = get_witness_schedule_object();
   vector<price> feeds; feeds.reserve( wso.current_shuffled_witnesses.size() );
   for( const auto& w : wso.current_shuffled_witnesses ) {
      const auto& wit = get_witness(w);
      if( wit.last_mbd_exchange_update < now + MUSE_MAX_FEED_AGE &&
          !wit.mbd_exchange_rate.is_null() )
      {
         feeds.push_back( wit.mbd_exchange_rate );
      }
   }

   if( feeds.size() >= MUSE_MIN_FEEDS ) {
      std::sort( feeds.begin(), feeds.end() );
      auto median_feed = feeds[feeds.size()/2];

      modify( get_feed_history(), [&]( feed_history_object& fho ){
         fho.price_history.push_back( median_feed );
         if( fho.price_history.size() > MUSE_FEED_HISTORY_WINDOW )
             fho.price_history.pop_front();

         if( fho.price_history.size() ) {
            std::deque<price> copy = fho.price_history;
            std::sort( copy.begin(), copy.end() ); /// todo: use nth_item
            fho.effective_median_history = fho.actual_median_history = copy[copy.size()/2];

            if( has_hardfork( MUSE_HARDFORK_0_6 ) )
            {
               // This block limits the effective median price to force MBD to remain at or
               // below 5% of the combined market cap of MUSE and MBD.
               //
               // For example, if we have 500 MUSE and 100 MBD, the price is limited to
               // 1900 MBD / 500 MUSE which works out to be $3.80.  At this price, 500 MUSE
               // would be valued at 500 * $3.80 = $1900.  100 MBD is by definition always $100,
               // so the combined market cap is $1900 + $100 = $2000.

               const auto& gpo = get_dynamic_global_properties();

               if( gpo.current_mbd_supply.amount > 0 )
               {
		  if( fho.effective_median_history.base.asset_id != MBD_SYMBOL )
		     fho.effective_median_history = ~fho.effective_median_history;
                  price max_price( asset( 19 * gpo.current_mbd_supply.amount, MBD_SYMBOL ), gpo.current_supply );

                  if( max_price > fho.effective_median_history )
                     fho.effective_median_history = max_price;
               }
            }
         }
      });
   }
} FC_CAPTURE_AND_RETHROW() }

void database::apply_transaction(const signed_transaction& trx, uint32_t skip)
{
   detail::with_skip_flags( *this, skip, [&]() { _apply_transaction(trx); });
}

void database::_apply_transaction(const signed_transaction& trx)
{ try {
   _current_trx_id = trx.id();
   uint32_t skip = get_node_properties().skip_flags;

   if( !(skip&skip_validate) )   /* issue #505 explains why this skip_flag is disabled */
      trx.validate();

   auto& trx_idx = get_mutable_index_type<transaction_index>();
   const chain_id_type& chain_id = MUSE_CHAIN_ID;
   auto trx_id = trx.id();
   FC_ASSERT( (skip & skip_transaction_dupe_check) ||
              trx_idx.indices().get<by_trx_id>().find(trx_id) == trx_idx.indices().get<by_trx_id>().end() );
   transaction_evaluation_state eval_state(this);
   eval_state._trx = &trx;

   if( !(skip & (skip_transaction_signatures | skip_authority_check) ) )
   {
      auto get_active  = [&]( const string& name ) { return &get_account(name).active; };
      auto get_owner   = [&]( const string& name ) { return &get_account(name).owner;  };
      auto get_basic = [&]( const string& name ) { return &get_account(name).basic;  };
      auto get_master_cont = [&]( const string& url ) { return &get_content(url).manage_master; };
      auto get_comp_cont = [&]( const string& url ) { return &get_content(url).manage_comp; };

      trx.verify_authority( chain_id, get_active, get_owner, get_basic, get_master_cont, get_comp_cont,
                            has_hardfork( MUSE_HARDFORK_0_4 ) ? 3 :
                            has_hardfork( MUSE_HARDFORK_0_3 ) ? 2 : 1 );
   }
   flat_set<string> required; vector<authority> other;
   flat_set<string> required_content;
   trx.get_required_authorities( required, required, required, required_content, required_content, other );
   auto trx_size = fc::raw::pack_size(trx);

   for( const auto& auth : required ) {
      const auto& acnt = get_account(auth);

      update_account_bandwidth( acnt, trx_size );
      for( const auto& op : trx.operations ) {
         if( is_market_operation( op ) )
         {
            update_account_market_bandwidth( get_account(auth), trx_size );
            break;
         }
      }
   }

   //Skip all manner of expiration and TaPoS checking if we're on block 1; It's impossible that the transaction is
   //expired, and TaPoS makes no sense as no blocks exist.
   if( BOOST_LIKELY(head_block_num() > 0) )
   {
      if( !(skip & skip_tapos_check) )
      {
         const auto& tapos_block_summary = block_summary_id_type( trx.ref_block_num )(*this);
         //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
         FC_ASSERT( trx.ref_block_prefix == tapos_block_summary.block_id._hash[1],
                    "", ("trx.ref_block_prefix", trx.ref_block_prefix)
                    ("tapos_block_summary",tapos_block_summary.block_id._hash[1]));
      }

      fc::time_point_sec now = head_block_time();

      FC_ASSERT( trx.expiration <= now + fc::seconds(MUSE_MAX_TIME_UNTIL_EXPIRATION), "",
                 ("trx.expiration",trx.expiration)("now",now)("max_til_exp",MUSE_MAX_TIME_UNTIL_EXPIRATION));
      FC_ASSERT( now < trx.expiration, "", ("now",now)("trx.exp",trx.expiration) );
      FC_ASSERT( now <= trx.expiration, "", ("now",now)("trx.exp",trx.expiration) );
   }

   //Insert transaction into unique transactions database.
   if( !(skip & skip_transaction_dupe_check) )
   {
      create<transaction_object>([&](transaction_object& transaction) {
         transaction.trx_id = trx_id;
         transaction.trx = trx;
      });
   }

   //Finally process the operations
   _current_op_in_trx = 0;
   for( const auto& op : trx.operations )
   { try {
      apply_operation(eval_state, op);
      ++_current_op_in_trx;
     } FC_CAPTURE_AND_RETHROW( (op) );
   }
   _current_trx_id = transaction_id_type();

} FC_CAPTURE_AND_RETHROW( (trx) ) }

void database::apply_operation(transaction_evaluation_state& eval_state, const operation& op)
{ try {
   int i_which = op.which();
   uint64_t u_which = uint64_t( i_which );
   FC_ASSERT( i_which >= 0, "Negative operation tag in operation ${op}", ("op",op) );
   FC_ASSERT( u_which < _operation_evaluators.size(), "No registered evaluator for operation ${op}", ("op",op) );
   unique_ptr<op_evaluator>& eval = _operation_evaluators[ u_which ];
   FC_ASSERT( eval, "No registered evaluator for operation ${op}", ("op",op) );
   push_applied_operation( op );
   eval->evaluate( eval_state, op, true );
   notify_post_apply_operation( op );
} FC_CAPTURE_AND_RETHROW(  ) }

const witness_object& database::validate_block_header( uint32_t skip, const signed_block& next_block )const
{
   FC_ASSERT( head_block_id() == next_block.previous, "", ("head_block_id",head_block_id())("next.prev",next_block.previous) );
   FC_ASSERT( head_block_time() < next_block.timestamp, "", ("head_block_time",head_block_time())("next",next_block.timestamp)("blocknum",next_block.block_num()) );
   const witness_object& witness = get_witness( next_block.witness ); //(*this);

   if( !(skip&skip_witness_signature) )
      FC_ASSERT( next_block.validate_signee( witness.signing_key ) );

   if( !(skip&skip_witness_schedule_check) )
   {
      uint32_t slot_num = get_slot_at_time( next_block.timestamp );
      FC_ASSERT( slot_num > 0 );

      string scheduled_witness = get_scheduled_witness( slot_num );

      FC_ASSERT( witness.owner == scheduled_witness, "Witness produced block at wrong time",
                 ("block witness",next_block.witness)("scheduled",scheduled_witness)("slot_num",slot_num) );
   }

   return witness;
}

void database::create_block_summary(const signed_block& next_block)
{
   block_summary_id_type sid(next_block.block_num() & 0xffff );
   modify( sid(*this), [&](block_summary_object& p) {
         p.block_id = next_block.id();
   });
}

void database::update_global_dynamic_data( const signed_block& b )
{
   auto block_size = fc::raw::pack_size(b);
   const dynamic_global_property_object& _dgp =
      dynamic_global_property_id_type(0)(*this);

   uint32_t missed_blocks = 0;
   if( head_block_time() != fc::time_point_sec() )
   {
      missed_blocks = get_slot_at_time( b.timestamp );
      assert( missed_blocks != 0 );
      missed_blocks--;
      for( uint32_t i = 0; i < missed_blocks; ++i )
      {
         const auto& witness_missed = get_witness( get_scheduled_witness( i+1 ) );
         if( witness_missed.owner != b.witness )
         {
            modify( witness_missed, [this]( witness_object& w )
            {
               w.total_missed++;
               if( has_hardfork( MUSE_HARDFORK_0_3 )
                   && head_block_num() - w.last_confirmed_block_num  > MUSE_BLOCKS_PER_DAY )
                  w.signing_key = public_key_type();
            } );
         }
      }
   }

   // dynamic global properties updating
   modify( _dgp, [&]( dynamic_global_property_object& dgp )
   {
      // This is constant time assuming 100% participation. It is O(B) otherwise (B = Num blocks between update)
      for( uint32_t i = 0; i < missed_blocks + 1; i++ )
      {
         dgp.participation_count -= dgp.recent_slots_filled.hi & 0x8000000000000000ULL ? 1 : 0;
         dgp.recent_slots_filled = ( dgp.recent_slots_filled << 1 ) + ( i == 0 ? 1 : 0 );
         dgp.participation_count += ( i == 0 ? 1 : 0 );
      }

      dgp.head_block_number = b.block_num();
      dgp.head_block_id = b.id();
      dgp.time = b.timestamp;
      dgp.current_aslot += missed_blocks+1;
      dgp.average_block_size = (99 * dgp.average_block_size + block_size)/100;

      /**
       *  About once per minute the average network use is consulted and used to
       *  adjust the reserve ratio. Anything above 50% usage reduces the ratio by
       *  half which should instantly bring the network from 50% to 25% use unless
       *  the demand comes from users who have surplus capacity. In other words,
       *  a 50% reduction in reserve ratio does not result in a 50% reduction in usage,
       *  it will only impact users who where attempting to use more than 50% of their
       *  capacity.
       *
       *  When the reserve ratio is at its max (10,000) a 50% reduction will take 3 to
       *  4 days to return back to maximum.  When it is at its minimum it will return
       *  back to its prior level in just a few minutes.
       *
       *  If the network reserve ratio falls under 100 then it is probably time to
       *  increase the capacity of the network.
       */
      if( dgp.head_block_number % 20 == 0 )
      {
         if( dgp.average_block_size > dgp.maximum_block_size / 4 ) 
         {
            dgp.current_reserve_ratio /= 2; /// exponential back up
         }
         else
         { /// linear growth... not much fine grain control near full capacity
            dgp.current_reserve_ratio++;
         }

         if( dgp.current_reserve_ratio > MUSE_MAX_RESERVE_RATIO )
            dgp.current_reserve_ratio = MUSE_MAX_RESERVE_RATIO;
      }
      dgp.max_virtual_bandwidth = (dgp.maximum_block_size * dgp.current_reserve_ratio *
                                  MUSE_BANDWIDTH_PRECISION * MUSE_BANDWIDTH_AVERAGE_WINDOW_SECONDS) / MUSE_BLOCK_INTERVAL;
   } );

   if( !(get_node_properties().skip_flags & skip_undo_history_check) )
   {
      MUSE_ASSERT( _dgp.head_block_number - _dgp.last_irreversible_block_num  < MUSE_MAX_UNDO_HISTORY, undo_database_exception,
                 "Please add a checkpoint if you would like to continue applying blocks beyond this point.",
                 ("last_irreversible_block_num",_dgp.last_irreversible_block_num)("head", _dgp.head_block_number)
                 ("max_undo",MUSE_MAX_UNDO_HISTORY) );
   }

   _undo_db.set_max_size( _dgp.head_block_number - _dgp.last_irreversible_block_num + 1 );
   _fork_db.set_max_size( _dgp.head_block_number - _dgp.last_irreversible_block_num + 1 );
}

void database::update_virtual_supply()
{
   modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& dgp )
   {
      dgp.virtual_supply = dgp.current_supply
         + ( get_feed_history().effective_median_history.is_null() ? asset( 0, MUSE_SYMBOL )
                : dgp.current_mbd_supply * get_feed_history().effective_median_history );
   });
}

class push_proposal_nesting_guard {
public:
   push_proposal_nesting_guard( uint32_t& nesting_counter )
      : orig_value(nesting_counter), counter(nesting_counter)
   {
      FC_ASSERT( counter < MUSE_MAX_MINERS * 2, "Max proposal nesting depth exceeded!" );
      counter++;
   }
   ~push_proposal_nesting_guard()
   {
      if( --counter != orig_value )
         elog( "Unexpected proposal nesting count value: ${n} != ${o}", ("n",counter)("o",orig_value) );
   }
private:
   const uint32_t  orig_value;
   uint32_t& counter;
};

void database::push_proposal(const proposal_object& proposal)
{ try {
   dlog( "Proposal: executing ${p}", ("p",proposal) );

   push_proposal_nesting_guard guard( _push_proposal_nesting_depth );

   if( _undo_db.size() >= _undo_db.max_size() )
      _undo_db.set_max_size( _undo_db.size() + 1 );

   auto session = _undo_db.start_undo_session(true);
   _current_op_in_trx = 0;
   transaction_evaluation_state eval_state(this);
   eval_state._is_proposed_trx = true;
   processed_transaction ptrx(proposal.proposed_transaction);

   eval_state._trx = &ptrx;

   for( const operation& op : proposal.proposed_transaction.operations )
   {
      try{
         apply_operation(eval_state,op);
         ++_current_op_in_trx;
      }FC_CAPTURE_AND_RETHROW( (op) );
   }
   remove(proposal);
   session.merge();
      
}FC_CAPTURE_AND_RETHROW( (proposal) ) }

void database::update_signing_witness(const witness_object& signing_witness, const signed_block& new_block)
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   uint64_t new_block_aslot = dpo.current_aslot + get_slot_at_time( new_block.timestamp );

   modify( signing_witness, [&]( witness_object& _wit )
   {
      _wit.last_aslot = new_block_aslot;
      _wit.last_confirmed_block_num = new_block.block_num();
   } );
}

void database::update_last_irreversible_block()
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();

   /**
    * Prior to voting taking over, we must be more conservative...
    *
    */
   if( head_block_num() < MUSE_START_MINER_VOTING_BLOCK )
   {
      modify( dpo, [&]( dynamic_global_property_object& _dpo )
      {
         if ( head_block_num() > MUSE_MAX_MINERS )
            _dpo.last_irreversible_block_num = head_block_num() - MUSE_MAX_MINERS;
      } );
      return;
   }

   const witness_schedule_object& wso = witness_schedule_id_type()(*this);

   vector< const witness_object* > wit_objs;
   wit_objs.reserve( wso.current_shuffled_witnesses.size() );
   for( const string& wid : wso.current_shuffled_witnesses )
      wit_objs.push_back( &get_witness(wid) );

   static_assert( MUSE_IRREVERSIBLE_THRESHOLD > 0, "irreversible threshold must be nonzero" );

   // 1 1 1 2 2 2 2 2 2 2 -> 2     .7*10 = 7
   // 1 1 1 1 1 1 1 2 2 2 -> 1
   // 3 3 3 3 3 3 3 3 3 3 -> 3

   size_t offset = ((MUSE_100_PERCENT - MUSE_IRREVERSIBLE_THRESHOLD) * wit_objs.size() / MUSE_100_PERCENT);

   std::nth_element( wit_objs.begin(), wit_objs.begin() + offset, wit_objs.end(),
      []( const witness_object* a, const witness_object* b )
      {
         return a->last_confirmed_block_num < b->last_confirmed_block_num;
      } );

   uint32_t new_last_irreversible_block_num = wit_objs[offset]->last_confirmed_block_num;

   if( new_last_irreversible_block_num > dpo.last_irreversible_block_num )
   {
      modify( dpo, [&]( dynamic_global_property_object& _dpo )
      {
         _dpo.last_irreversible_block_num = new_last_irreversible_block_num;
      } );
   }
}


bool database::apply_order( const limit_order_object& new_order_object )
{
   auto order_id = new_order_object.id;
  
   const auto& limit_price_idx = get_index_type<limit_order_index>().indices().get<by_price>();

   auto max_price = ~new_order_object.sell_price;
   auto limit_itr = limit_price_idx.lower_bound(max_price.max());
   auto limit_end = limit_price_idx.upper_bound(max_price);

   bool finished = false;
   while( !finished && limit_itr != limit_end )
   {
      auto old_limit_itr = limit_itr;
      ++limit_itr;
      // match returns 2 when only the old order was fully filled. In this case, we keep matching; otherwise, we stop.
      finished = ( match(new_order_object, *old_limit_itr, old_limit_itr->sell_price) & 0x1 );
   }
   
   return find_object(order_id) == nullptr;
}

int database::match( const limit_order_object& new_order, const limit_order_object& old_order, const price& match_price )
{
   assert( new_order.sell_price.quote.asset_id == old_order.sell_price.base.asset_id );
   assert( new_order.sell_price.base.asset_id  == old_order.sell_price.quote.asset_id );
   assert( new_order.for_sale > 0 && old_order.for_sale > 0 );
   assert( match_price.quote.asset_id == new_order.sell_price.base.asset_id );
   assert( match_price.base.asset_id == old_order.sell_price.base.asset_id );

   auto new_order_for_sale = new_order.amount_for_sale();
   auto old_order_for_sale = old_order.amount_for_sale();

   asset new_order_pays, new_order_receives, old_order_pays, old_order_receives;

   if( new_order_for_sale <= old_order_for_sale * match_price )
   {
      old_order_receives = new_order_for_sale;
      new_order_receives  = new_order_for_sale * match_price;
   }
   else
   {
      //This line once read: assert( old_order_for_sale < new_order_for_sale * match_price );
      //This assert is not always true -- see trade_amount_equals_zero in operation_tests.cpp
      //Although new_order_for_sale is greater than old_order_for_sale * match_price, old_order_for_sale == new_order_for_sale * match_price
      //Removing the assert seems to be safe -- apparently no asset is created or destroyed.
      new_order_receives = old_order_for_sale;
      old_order_receives = old_order_for_sale * match_price;
   }

   old_order_pays = new_order_receives;
   new_order_pays = old_order_receives;

   assert( new_order_pays == new_order.amount_for_sale() ||
           old_order_pays == old_order.amount_for_sale() );

   push_applied_operation( fill_order_operation( new_order.seller, new_order.orderid, new_order_pays, old_order.seller, old_order.orderid, old_order_pays ) );

   int result = 0;
   result |= fill_order( new_order, new_order_pays, new_order_receives );
   result |= fill_order( old_order, old_order_pays, old_order_receives ) << 1;
   assert( result != 0 );
   return result;
}


bool database::fill_order( const limit_order_object& order, const asset& pays, const asset& receives )
{
   try
   {
      FC_ASSERT( order.amount_for_sale().asset_id == pays.asset_id );
      FC_ASSERT( pays.asset_id != receives.asset_id );

      const account_object& seller = get_account( order.seller );

      adjust_balance( seller, receives );

      if( pays == order.amount_for_sale() )
      {
         remove( order );
         return true;
      }
      else
      {
         modify( order, [&]( limit_order_object& b )
         {
            b.for_sale -= pays.amount;
         } );
         /**
          *  There are times when the AMOUNT_FOR_SALE * SALE_PRICE == 0 which means that we
          *  have hit the limit where the seller is asking for nothing in return.  When this
          *  happens we must refund any balance back to the seller, it is too small to be
          *  sold at the sale price.
          */
         if( order.amount_to_receive().amount == 0 )
         {
            cancel_order(order);
            return true;
         }
         return false;
      }
   }
   FC_CAPTURE_AND_RETHROW( (order)(pays)(receives) )
}

void database::cancel_order( const limit_order_object& order )
{
   adjust_balance( get_account(order.seller), order.amount_for_sale() );
   remove(order);
}


void database::clear_expired_transactions()
{
   //Look for expired transactions in the deduplication list, and remove them.
   //Transactions must have expired by at least two forking windows in order to be removed.
   auto& transaction_idx = static_cast<transaction_index&>(get_mutable_index(implementation_ids, impl_transaction_object_type));
   const auto& dedupe_index = transaction_idx.indices().get<by_expiration>();
   while( (!dedupe_index.empty()) && (head_block_time() > dedupe_index.begin()->trx.expiration) )
      transaction_idx.remove(*dedupe_index.begin());
}

void database::clear_expired_orders()
{
   auto now = head_block_time();
   const auto& orders_by_exp = get_index_type<limit_order_index>().indices().get<by_expiration>();
   auto itr = orders_by_exp.begin();
   while( itr != orders_by_exp.end() && itr->expiration < now )
   {
      cancel_order( *itr );
      itr = orders_by_exp.begin();
   }
}

void database::clear_expired_delegations()
{
   const auto dgpo = get_dynamic_global_properties();
   auto now = dgpo.time;
   const auto& delegations_by_exp = get_index_type< vesting_delegation_expiration_index >().indices().get< by_expiration >();
   auto itr = delegations_by_exp.begin();
   while( itr != delegations_by_exp.end() && itr->expiration < now )
   {
      if( find_streaming_platform( itr->delegator ) )
         modify( dgpo, [&itr]( dynamic_global_property_object& dgpo ) {
            dgpo.total_vested_by_platforms += itr->vesting_shares.amount;
         });

      modify( get_account( itr->delegator ), [&]( account_object& a )
      {
         a.delegated_vesting_shares -= itr->vesting_shares;
      });

      push_applied_operation( return_vesting_delegation_operation( itr->delegator, itr->vesting_shares ) );

      remove( *itr );
      itr = delegations_by_exp.begin();
   }
}

void database::clear_expired_proposals()
{
   if ( !has_hardfork(MUSE_HARDFORK_0_3) ) return;

   const auto& proposal_expiration_index = get_index_type<proposal_index>().indices().get<by_expiration>();
   while( !proposal_expiration_index.empty() && proposal_expiration_index.begin()->expiration_time <= head_block_time() )
   {
      const proposal_object& proposal = *proposal_expiration_index.begin();
      try {
         if( proposal.is_authorized_to_execute(*this) )
         {
            push_proposal(proposal);
            continue;
         }
      } catch( const fc::exception& e ) {
         ilog("Failed to apply proposed transaction on its expiration. Deleting it.\n${proposal}\n${error}",
              ("proposal", proposal)("error", e.to_detail_string()));
      }
      remove(proposal);
   }
}

string database::to_pretty_string( const asset& a )const
{
   return a.asset_id(*this).amount_to_pretty_string(a.amount);
}

void database::adjust_balance( const account_object& a, const asset& delta )
{
   if( delta.asset_id == MUSE_SYMBOL || delta.asset_id == MBD_SYMBOL )
      modify( a, [&]( account_object& acnt )   
      {
         if(delta.asset_id==MUSE_SYMBOL)
         {
            acnt.balance.amount += delta.amount;
            return;
         }
         if(delta.asset_id==MBD_SYMBOL)
         {
            if( a.mbd_seconds_last_update != head_block_time() )
            {
               acnt.mbd_seconds += fc::uint128_t(a.mbd_balance.amount.value) * (head_block_time() - a.mbd_seconds_last_update).to_seconds();
               acnt.mbd_seconds_last_update = head_block_time();
               if( acnt.mbd_seconds > 0 && (acnt.mbd_seconds_last_update - acnt.mbd_last_interest_payment).to_seconds() > MUSE_SBD_INTEREST_COMPOUND_INTERVAL_SEC )
               {
                  auto interest = acnt.mbd_seconds / MUSE_SECONDS_PER_YEAR;
                  interest *= get_dynamic_global_properties().mbd_interest_rate;
                  interest /= MUSE_100_PERCENT;
                  asset interest_paid(interest.to_uint64(), MBD_SYMBOL);
                  acnt.mbd_balance += interest_paid;
                  acnt.mbd_seconds = 0;
                  acnt.mbd_last_interest_payment = head_block_time();
                  push_applied_operation( interest_operation( a.name, interest_paid ) );
                  modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& props)
                  {
                     props.current_mbd_supply += interest_paid;
                     props.virtual_supply += interest_paid * get_feed_history().effective_median_history;
                  } );
               }
            }
            acnt.mbd_balance += delta;
         }
      });
   else try {
      if( delta.amount == 0 )
         return;
       
      auto& index = get_index_type<account_balance_index>().indices().get<by_account_asset>();
      auto itr = index.find(boost::make_tuple(a.get_id(), delta.asset_id));
      if(itr == index.end())
      {
         FC_ASSERT( delta.amount > 0, "Insufficient Balance: ${a}'s balance of ${b} is less than required ${r}",
               ("a",a.name)
               ("b",to_pretty_string(asset(0,delta.asset_id)))
               ("r",to_pretty_string(-delta)));
         create<account_balance_object>([a,&delta](account_balance_object& b) {
               b.owner = a.get_id();
               b.asset_type = delta.asset_id;
               b.balance = delta.amount.value;
         });
      }else{
         if( delta.amount < 0 )
            FC_ASSERT( itr->get_balance() >= -delta, "Insufficient Balance: ${a}'s balance of ${b} is less than required ${r}", ("a",a.name)("b",to_pretty_string(itr->get_balance()))("r",to_pretty_string(-delta)));
         modify(*itr, [delta](account_balance_object& b) {
               b.adjust_balance(delta);
         });
      }
   }
FC_CAPTURE_AND_RETHROW( (a)(delta) ) }

void database::adjust_supply( const asset& delta, bool adjust_vesting )
{

   const auto& props = get_dynamic_global_properties();
   if( props.head_block_number < MUSE_BLOCKS_PER_DAY*7 )
      adjust_vesting = false;

   modify( props, [&]( dynamic_global_property_object& props )
   {
      if(delta.asset_id == MUSE_SYMBOL){
         asset new_vesting( (adjust_vesting && delta.amount > 0) ? delta.amount * 9 : 0, MUSE_SYMBOL );
         props.current_supply += delta + new_vesting;
         props.virtual_supply += delta + new_vesting;
         props.total_vesting_fund_muse += new_vesting;
         assert( props.current_supply.amount.value >= 0 );
      }else if (delta.asset_id == MBD_SYMBOL){
         props.current_mbd_supply += delta;
         props.virtual_supply = props.current_mbd_supply * get_feed_history().effective_median_history + props.current_supply;
         assert( props.current_mbd_supply.amount.value >= 0 );
      }else
         FC_ASSERT( !"invalid symbol" );
   } );
}

const asset_object& database::get_asset(const std::string& symbol)const {
   auto& index = get_index_type<asset_index>().indices().get<by_symbol>();
   auto itr = index.find(symbol);
   FC_ASSERT(itr != index.end(), "Asset '${s}' not found", ("s",symbol));
   return *itr;
}

asset database::get_balance( const account_object& a, asset_id_type symbol )const
{
   if(symbol == MUSE_SYMBOL)
      return a.balance;
   if(symbol == MBD_SYMBOL)
      return a.mbd_balance;
   auto& index = get_index_type<account_balance_index>().indices().get<by_account_asset>();
   auto itr = index.find(boost::make_tuple(a.get_id(), symbol));
   if( itr == index.end() )
      return asset(0, symbol);
   return itr->get_balance();
}

void database::init_hardforks()
{
   _hardfork_times[ 0 ] = fc::time_point_sec( MUSE_GENESIS_TIME );
   _hardfork_versions[ 0 ] = hardfork_version( 0, 0 );
   FC_ASSERT( MUSE_HARDFORK_0_1 == 1, "Invalid hardfork configuration" );
   _hardfork_times[ MUSE_HARDFORK_0_1 ] = fc::time_point_sec( MUSE_HARDFORK_0_1_TIME );
   _hardfork_versions[ MUSE_HARDFORK_0_1 ] = MUSE_HARDFORK_0_1_VERSION;
   FC_ASSERT( MUSE_HARDFORK_0_2 == 2, "Invalid hardfork configuration" );
   _hardfork_times[ MUSE_HARDFORK_0_2 ] = fc::time_point_sec( MUSE_HARDFORK_0_2_TIME );
   _hardfork_versions[ MUSE_HARDFORK_0_2 ] = MUSE_HARDFORK_0_2_VERSION;
   FC_ASSERT( MUSE_HARDFORK_0_3 == 3, "Invalid hardfork configuration" );
   _hardfork_times[ MUSE_HARDFORK_0_3 ] = fc::time_point_sec( MUSE_HARDFORK_0_3_TIME );
   _hardfork_versions[ MUSE_HARDFORK_0_3 ] = MUSE_HARDFORK_0_3_VERSION;
   FC_ASSERT( MUSE_HARDFORK_0_4 == 4, "Invalid hardfork configuration" );
   _hardfork_times[ MUSE_HARDFORK_0_4 ] = fc::time_point_sec( MUSE_HARDFORK_0_4_TIME );
   _hardfork_versions[ MUSE_HARDFORK_0_4 ] = MUSE_HARDFORK_0_4_VERSION;
   FC_ASSERT( MUSE_HARDFORK_0_5 == 5, "Invalid hardfork configuration" );
   _hardfork_times[ MUSE_HARDFORK_0_5 ] = fc::time_point_sec( MUSE_HARDFORK_0_5_TIME );
   _hardfork_versions[ MUSE_HARDFORK_0_5 ] = MUSE_HARDFORK_0_5_VERSION;
   FC_ASSERT( MUSE_HARDFORK_0_6 == 6, "Invalid hardfork configuration" );
   _hardfork_times[ MUSE_HARDFORK_0_6 ] = fc::time_point_sec( MUSE_HARDFORK_0_6_TIME );
   _hardfork_versions[ MUSE_HARDFORK_0_6 ] = MUSE_HARDFORK_0_6_VERSION;

   const auto& hardforks = hardfork_property_id_type()( *this );
   FC_ASSERT( hardforks.last_hardfork <= MUSE_NUM_HARDFORKS, "Chain knows of more hardforks than configuration",
              ("hardforks.last_hardfork",hardforks.last_hardfork)("MUSE_NUM_HARDFORKS",MUSE_NUM_HARDFORKS) );
   FC_ASSERT( _hardfork_versions[ hardforks.last_hardfork ] <= MUSE_BLOCKCHAIN_VERSION, "Blockchain version is older than last applied hardfork" );
}

void database::reset_virtual_schedule_time()
{
   const witness_schedule_object& wso = witness_schedule_id_type()(*this);
   modify( wso, [&](witness_schedule_object& o )
   {
       o.current_virtual_time = fc::uint128(); // reset it 0
   } );

   const auto& idx = get_index_type<witness_index>().indices();
   for( const auto& witness : idx )
   {
      modify( witness, [&]( witness_object& wobj )
      {
         wobj.virtual_position = fc::uint128();
         wobj.virtual_last_update = wso.current_virtual_time;
         wobj.virtual_scheduled_time = VIRTUAL_SCHEDULE_LAP_LENGTH2 / (wobj.votes.value+1);
      } );
   }
}

void database::process_hardforks()
{
   try
   {
      // If there are upcoming hardforks and the next one is later, do nothing
      const auto& hardforks = hardfork_property_id_type()( *this );

      while( _hardfork_versions[ hardforks.last_hardfork ] < hardforks.next_hardfork
         && hardforks.next_hardfork_time <= head_block_time() )
      {
         if( hardforks.last_hardfork < MUSE_NUM_HARDFORKS )
            apply_hardfork( hardforks.last_hardfork + 1 );
         else
            throw unknown_hardfork_exception();
      }
   }
   FC_CAPTURE_AND_RETHROW()
}

bool database::has_hardfork( uint32_t hardfork )const
{
   uint32_t processed_hardforks = hardfork_property_id_type()( *this ).processed_hardforks.size();
   return processed_hardforks > hardfork;
}

void database::set_hardfork( uint32_t hardfork, bool apply_now )
{
   auto const& hardforks = hardfork_property_id_type()( *this );

   for( uint32_t i = hardforks.last_hardfork + 1; i <= hardfork && i <= MUSE_NUM_HARDFORKS; i++ )
   {
      modify( hardforks, [i,this]( hardfork_property_object& hpo )
      {
         hpo.next_hardfork = _hardfork_versions[i];
         hpo.next_hardfork_time = head_block_time();
      } );

      if( apply_now )
         apply_hardfork( i );
   }
}

void database::apply_hardfork( uint32_t hardfork )
{
   ilog("Applying hardfork ${i} at #${n} / ${t}", ("i",hardfork)("n",head_block_num())("t",head_block_time()) );

   switch( hardfork )
   {
      case MUSE_HARDFORK_0_1:
         {
            // This is for unit tests only. Evil.
            const auto& initminer = get_account( MUSE_INIT_MINER_NAME );
            if ( initminer.balance.amount.value >= 10 * asset::static_precision() ) // not true in mainnet
            {
               custom_operation test_op;
               string op_msg = "Test: Hardfork applied";
               test_op.data = vector< char >( op_msg.begin(), op_msg.end() );
               test_op.required_auths.insert( MUSE_INIT_MINER_NAME );
               push_applied_operation( test_op );
            }
         }
         break;

      case MUSE_HARDFORK_0_2:
         {
            const auto& gpo = get_dynamic_global_properties();
            modify( gpo, []( dynamic_global_property_object& dgpo ) {
               dgpo.current_supply += dgpo.supply_delta;
               dgpo.virtual_supply += dgpo.supply_delta;
               dgpo.supply_delta = asset();
            } );
         }
         break;

      case MUSE_HARDFORK_0_3:
         {
            const auto& proposal_expiration_index = get_index_type<proposal_index>().indices().get<by_expiration>();
            while( !proposal_expiration_index.empty() && proposal_expiration_index.begin()->expiration_time <= head_block_time() )
               remove( *proposal_expiration_index.begin() );
         }
         break;

      default:
         break;
   }

   modify( hardfork_property_id_type()( *this ), [hardfork,this]( hardfork_property_object& hfp )
   {
      FC_ASSERT( hardfork == hfp.last_hardfork + 1, "Hardfork being applied out of order", ("hardfork",hardfork)("hfp.last_hardfork",hfp.last_hardfork) );
      FC_ASSERT( hfp.processed_hardforks.size() == hardfork, "Hardfork being applied out of order" );
      hfp.processed_hardforks.push_back( _hardfork_times[ hardfork ] );
      hfp.last_hardfork = hardfork;
      hfp.current_hardfork_version = _hardfork_versions[ hardfork ];
      FC_ASSERT( hfp.processed_hardforks[ hfp.last_hardfork ] == _hardfork_times[ hfp.last_hardfork ], "Hardfork processing failed sanity check..." );
   } );
}

void database::retally_liquidity_weight() {
   const auto& ridx = get_index_type<liquidity_reward_index>().indices().get<by_owner>();
   for( const auto& i : ridx ) {
      modify( i, []( liquidity_reward_balance_object& o ){
         o.update_weight(true/*HAS HARDFORK10 if this method is called*/);
      });
   }
}

/**
 * Verifies all supply invariantes check out
 */
void database::validate_invariants()const
{
   if( !has_hardfork(MUSE_HARDFORK_0_2) ) return; // total_supply tracking is incorrect before HF2
   try
   {
      const auto& account_idx = get_index_type<account_index>().indices().get<by_name>();
      asset total_supply = asset( 0, MUSE_SYMBOL );
      asset total_mbd = asset( 0, MBD_SYMBOL );
      asset total_vesting = asset( 0, VESTS_SYMBOL );
      share_type total_vsf_votes = share_type( 0 );

      auto gpo = get_dynamic_global_properties();

      /// verify no witness has too many votes
      const auto& witness_idx = get_index_type< witness_index >().indices();
      for( auto itr = witness_idx.begin(); itr != witness_idx.end(); ++itr )
         FC_ASSERT( itr->votes < gpo.total_vesting_shares.amount, "", ("itr",*itr) );

      share_type total_vested_by_sp = 0;
      for( auto itr = account_idx.begin(); itr != account_idx.end(); ++itr )
      {
         total_supply += itr->balance;
         total_mbd += itr->mbd_balance;
         total_vesting += itr->vesting_shares;
         total_vsf_votes += ( itr->proxy == MUSE_PROXY_TO_SELF_ACCOUNT ?
                                 itr->witness_vote_weight() :
                                 ( MUSE_MAX_PROXY_RECURSION_DEPTH > 0 ?
                                      itr->proxied_vsf_votes[MUSE_MAX_PROXY_RECURSION_DEPTH - 1] :
                                      itr->vesting_shares.amount ) );
         if( find_streaming_platform( itr->name ) != nullptr )
            total_vested_by_sp += itr->vesting_shares.amount - itr->delegated_vesting_shares.amount
                                  + itr->received_vesting_shares.amount;
      }

      const auto& convert_request_idx = get_index_type< convert_index >().indices();

      for( auto itr = convert_request_idx.begin(); itr != convert_request_idx.end(); ++itr )
      {
         if( itr->amount.asset_id == MUSE_SYMBOL )
            total_supply += itr->amount;
         else if( itr->amount.asset_id == MBD_SYMBOL )
            total_mbd += itr->amount;
         else
            FC_ASSERT( !"Encountered illegal symbol in convert_request_object" );
      }

      const auto& limit_order_idx = get_index_type< limit_order_index >().indices();

      for( auto itr = limit_order_idx.begin(); itr != limit_order_idx.end(); ++itr )
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

      const auto& balances = get_index_type< balance_index >().indices();
      for( auto itr = balances.begin(); itr != balances.end(); itr++ )
         total_supply += itr->balance;

      const auto& content_idx = get_index_type< content_index >().indices();
      for( auto itr = content_idx.begin(); itr != content_idx.end(); itr++ )
      {
         total_supply += itr->accumulated_balance_master;
         total_supply += itr->accumulated_balance_comp;
      }

      total_supply += gpo.total_vesting_fund_muse;

      FC_ASSERT( gpo.current_supply == total_supply, "", ("gpo.current_supply",gpo.current_supply)("total_supply",total_supply) );
      FC_ASSERT( gpo.current_mbd_supply == total_mbd, "", ("gpo.current_mbd_supply",gpo.current_mbd_supply)("total_mbd",total_mbd) );
      FC_ASSERT( gpo.total_vesting_shares == total_vesting, "", ("gpo.total_vesting_shares",gpo.total_vesting_shares)("total_vesting",total_vesting) );
      FC_ASSERT( gpo.total_vesting_shares.amount == total_vsf_votes, "", ("total_vesting_shares",gpo.total_vesting_shares)("total_vsf_votes",total_vsf_votes) );
      FC_ASSERT( gpo.total_vested_by_platforms == total_vested_by_sp, "",
                 ("total_vested_by_platforms",gpo.total_vested_by_platforms)
                 ("total_vested_by_sp",total_vested_by_sp) );

      FC_ASSERT( gpo.virtual_supply >= gpo.current_supply );
      if ( !get_feed_history().effective_median_history.is_null() )
      {
         FC_ASSERT( gpo.current_mbd_supply * get_feed_history().effective_median_history + gpo.current_supply
            == gpo.virtual_supply, "", ("gpo.current_mbd_supply",gpo.current_mbd_supply)
                 ("get_feed_history().effective_median_history",get_feed_history().effective_median_history)
                 ("gpo.current_supply",gpo.current_supply)("gpo.virtual_supply",gpo.virtual_supply) );
      }
   }
   FC_CAPTURE_LOG_AND_RETHROW( (head_block_num()) );
}

void database::perform_vesting_share_split( uint32_t magnitude )
{
   //TODO_MUSE - recalculate score here
   try
   {
      modify( get_dynamic_global_properties(), [&]( dynamic_global_property_object& d )
      {
         d.total_vesting_shares.amount *= magnitude;
      } );

      // Need to update all VESTS in accounts and the total VESTS in the dgpo
      for( const auto& account : get_index_type<account_index>().indices() )
      {
         modify( account, [&]( account_object& a )
         {
            a.vesting_shares.amount *= magnitude;
            a.withdrawn             *= magnitude;
            a.to_withdraw           *= magnitude;
            a.vesting_withdraw_rate  = asset( a.to_withdraw / MUSE_VESTING_WITHDRAW_INTERVALS, VESTS_SYMBOL );
            if( a.vesting_withdraw_rate.amount == 0 )
               a.vesting_withdraw_rate.amount = 1;

            for( uint32_t i = 0; i < MUSE_MAX_PROXY_RECURSION_DEPTH; ++i )
               a.proxied_vsf_votes[i] *= magnitude;
         } );
      }

   }
   FC_CAPTURE_AND_RETHROW()
}


void database::retally_witness_vote_counts( bool force )
{
   const auto& account_idx = get_index_type< account_index >().indices();

   // Check all existing votes by account
   for( auto itr = account_idx.begin(); itr != account_idx.end(); ++itr )
   {
      const auto& a = *itr;
      uint16_t witnesses_voted_for = 0;
      if( force || (a.proxy != MUSE_PROXY_TO_SELF_ACCOUNT  ) )
      {
        const auto& vidx = get_index_type<witness_vote_index>().indices().get<by_account_witness>();
        auto wit_itr = vidx.lower_bound( boost::make_tuple( a.get_id(), witness_id_type() ) );
        while( wit_itr != vidx.end() && wit_itr->account == a.get_id() )
        {
           ++witnesses_voted_for;
           ++wit_itr;
        }
      }
      if( a.witnesses_voted_for != witnesses_voted_for )
      {
         modify( a, [&]( account_object& account )
         {
            account.witnesses_voted_for = witnesses_voted_for;
         } );
      }
   }
}

uint64_t database::get_scoring(const account_object& ao ) const
{
   uint64_t score = detail::isqrt(ao.get_scoring_vesting());
   for ( const auto & a : ao.friends ) {
      const auto& f = get<account_object>(a);
      score += detail::isqrt(f.get_scoring_vesting()) * MUSE_1ST_LEVEL_SCORING_PERCENTAGE / 100;
   }
   for ( const auto & a : ao.second_level ){
      const auto& sl = get<account_object>( a );
      score += detail::isqrt(sl.get_scoring_vesting()) * MUSE_2ST_LEVEL_SCORING_PERCENTAGE / 100;
   }
   return score;
}

uint64_t database::get_scoring(const content_object& co ) const
{
   uint32_t count=0;
   uint64_t score=0;
   for(auto&& d:co.distributions_comp ){
      ++count;
      score+=get_account(d.payee).score;
   }
   for(auto&& d:co.distributions_master ){
      ++count;
      score+=get_account(d.payee).score;
   }
   if(count)
      score /= count;
   else
      score = 0;
   return score;
}

void database::recursive_recalculate_score(const account_object& a, share_type delta)
{
   share_type old_amount = a.vesting_shares.amount - delta;
   int64_t score_delta = ((int64_t) detail::isqrt(a.get_scoring_vesting())) - detail::isqrt(old_amount.value);

   modify<account_object>(a,[score_delta](account_object& ao){
        ao.score += score_delta;
   });

   for( auto &f:a.friends ) {
      const auto& f_object = get<account_object>(f);
      modify<account_object>(f_object,[score_delta](account_object& ao){
           ao.score += score_delta * MUSE_1ST_LEVEL_SCORING_PERCENTAGE / 100;
      });
   }

   for( auto &f:a.second_level ) {
      const auto& f_object = get<account_object>(f);
      modify<account_object>(f_object,[score_delta](account_object& ao){
           ao.score += score_delta * MUSE_2ST_LEVEL_SCORING_PERCENTAGE / 100;
      });
   }
}

void database::recalculate_score(const account_object& a) {
   uint64_t score = detail::isqrt(a.get_scoring_vesting());

   for( auto &f:a.friends ) {
      const auto& f_object = get<account_object>(f);
      score += detail::isqrt(f_object.get_scoring_vesting()) * MUSE_1ST_LEVEL_SCORING_PERCENTAGE / 100;
   }

   for( auto &f:a.second_level ) {
      const auto& f_object = get<account_object>(f);
      score += detail::isqrt(f_object.get_scoring_vesting()) * MUSE_2ST_LEVEL_SCORING_PERCENTAGE / 100;
   }
   modify<account_object>(a,[&](account_object& ao){
        ao.score = score;
   });
};

namespace detail {
uint32_t isqrt(uint64_t a) {
   uint64_t rem = 0;
   uint32_t root = 0;
   uint32_t i;

   for( i = 0; i < 32; i++ ) {
      root <<= 1;
      rem <<= 2;
      rem += a >> 62;
      a <<= 2;

      if( root < rem ) {
         root++;
         rem -= root;
         root++;
      }
   }

   return (uint32_t) (root >> 1);
}
}//detail
} } //muse::chain
