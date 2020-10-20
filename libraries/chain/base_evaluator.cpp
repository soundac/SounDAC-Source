#include <muse/chain/database.hpp>
#include <muse/chain/base_evaluator.hpp>
#include <muse/chain/base_objects.hpp>

#ifndef IS_LOW_MEM
#include <boost/locale/encoding_utf.hpp>

using boost::locale::conv::utf_to_utf;

std::wstring utf8_to_wstring(const std::string& str)
{
    return utf_to_utf<wchar_t>(str.c_str(), str.c_str() + str.size());
}

std::string wstring_to_utf8(const std::wstring& str)
{
    return utf_to_utf<char>(str.c_str(), str.c_str() + str.size());
}

#endif

#include <fc/uint128.hpp>
#include <fc/utf8.hpp>

#include <limits>

namespace muse { namespace chain {
   using fc::uint128_t;

void witness_update_evaluator::do_apply( const witness_update_operation& o )
{
   db().get_account( o.owner ); // verify owner exists

   FC_ASSERT( o.url.size() <= MUSE_MAX_WITNESS_URL_LENGTH ); // TODO: move to validate after HF


   const auto& by_witness_name_idx = db().get_index_type< witness_index >().indices().get< by_name >();
   auto wit_itr = by_witness_name_idx.find( o.owner );
   if( wit_itr != by_witness_name_idx.end() )
   {
      db().modify( *wit_itr, [&]( witness_object& w ) {
         w.url                = o.url;
         w.signing_key        = o.block_signing_key;
         w.props              = o.props;
      });
   }
   else
   {
      db().create< witness_object >( [&]( witness_object& w ) {
         w.owner              = o.owner;
         w.url                = o.url;
         w.signing_key        = o.block_signing_key;
         w.created            = db().head_block_time();
         w.props              = o.props;
      });
   }
}


void account_create_evaluator::do_apply( const account_create_operation& o )
{
   if ( o.json_metadata.size() > 0 ) // TODO: move to validate after HF
   {
      FC_ASSERT( fc::json::is_valid(o.json_metadata), "JSON Metadata not valid JSON" );
   }

   const auto& creator = db().get_account( o.creator );

   const auto& props = db().get_dynamic_global_properties();

   if( db().head_block_num() > 0 ){
      FC_ASSERT( creator.balance >= o.fee, "Insufficient balance to create account", ( "creator.balance", creator.balance )( "required", o.fee ) );

      const witness_schedule_object& wso = db().get_witness_schedule_object();
      FC_ASSERT( o.fee >= wso.median_props.account_creation_fee, "Insufficient Fee: ${f} required, ${p} provided",
              ("f", wso.median_props.account_creation_fee)
              ("p", o.fee) );
   }

   db().modify( creator, [&o]( account_object& c ){
      c.balance -= o.fee;
   });

   const auto& new_account = db().create< account_object >( [&o,&props]( account_object& acc )
   {
      acc.name = o.new_account_name;
      acc.owner = o.owner;
      acc.active = o.active;
      acc.basic = o.basic;
      acc.memo_key = o.memo_key;
      acc.last_owner_update = fc::time_point_sec::min();
      acc.created = props.time;
      acc.last_vote_time = props.time;

      acc.recovery_account = o.creator;

#ifndef IS_LOW_MEM
      acc.json_metadata = o.json_metadata;
#endif
   });

   if( o.fee.amount > 0 )
      db().create_vesting( new_account, o.fee );
}

void account_create_with_delegation_evaluator::do_apply( const account_create_with_delegation_operation& o )
{
   auto& _db = db();

   FC_ASSERT( _db.has_hardfork( MUSE_HARDFORK_0_4 ), "Account creation with delegation is only allowed after hardfork 0.4" );

   const auto& creator = _db.get_account( o.creator );
   const auto& props = _db.get_dynamic_global_properties();
   const witness_schedule_object& wso = _db.get_witness_schedule_object();

   FC_ASSERT( creator.balance >= o.fee, "Insufficient balance to create account.",
               ( "creator.balance", creator.balance )
               ( "required", o.fee ) );

   FC_ASSERT( creator.vesting_shares - creator.delegated_vesting_shares - asset( creator.to_withdraw - creator.withdrawn, VESTS_SYMBOL ) >= o.delegation, "Insufficient vesting shares to delegate to new account.",
               ( "creator.vesting_shares", creator.vesting_shares )
               ( "creator.delegated_vesting_shares", creator.delegated_vesting_shares )( "required", o.delegation ) );

   auto target_delegation = asset( wso.median_props.account_creation_fee.amount * MUSE_CREATE_ACCOUNT_DELEGATION_RATIO, MUSE_SYMBOL ) * props.get_vesting_share_price();

   auto current_delegation = asset( o.fee.amount * MUSE_CREATE_ACCOUNT_DELEGATION_RATIO, MUSE_SYMBOL ) * props.get_vesting_share_price() + o.delegation;

   FC_ASSERT( current_delegation >= target_delegation, "Insufficient delegation ${f} required, ${p} provided.",
               ("f", target_delegation )
               ( "p", current_delegation )
               ( "account_creation_fee", wso.median_props.account_creation_fee )
               ( "o.fee", o.fee )
               ( "o.delegation", o.delegation ) );

   for( const auto& a : o.owner.account_auths )
   {
      _db.get_account( a.first );
   }

   for( const auto& a : o.active.account_auths )
   {
      _db.get_account( a.first );
   }

   for( const auto& a : o.basic.account_auths )
   {
      _db.get_account( a.first );
   }

   _db.modify( creator, [&o]( account_object& c )
   {
      c.balance -= o.fee;
      c.delegated_vesting_shares += o.delegation;
   });

   const auto& new_account = _db.create< account_object >( [&o,&props]( account_object& acc )
   {
      acc.name = o.new_account_name;
      acc.owner = o.owner;
      acc.active = o.active;
      acc.basic = o.basic;
      acc.memo_key = o.memo_key;
      acc.last_owner_update = fc::time_point_sec::min();
      acc.created = props.time;
      acc.last_vote_time = props.time;
      acc.received_vesting_shares = o.delegation;

      #ifndef IS_LOW_MEM
         acc.json_metadata = o.json_metadata;
      #endif
   });

   if( o.delegation.amount > 0 )
   {
      _db.create< vesting_delegation_object >( [&o,&_db]( vesting_delegation_object& vdo )
      {
         vdo.delegator = o.creator;
         vdo.delegatee = o.new_account_name;
         vdo.vesting_shares = o.delegation;
         vdo.min_delegation_time = _db.head_block_time() + MUSE_CREATE_ACCOUNT_DELEGATION_TIME;
      });
   }

   if( o.fee.amount > 0 )
      _db.create_vesting( new_account, o.fee );
}

void account_update_evaluator::do_apply( const account_update_operation& o )
{
   if ( o.json_metadata.size() > 0 ) // TODO: move to validate after HF
   {
      FC_ASSERT( fc::json::is_valid(o.json_metadata), "JSON Metadata not valid JSON" );
   }

   FC_ASSERT( o.account != MUSE_TEMP_ACCOUNT );

   const auto& account = db().get_account( o.account );

   if( o.owner )
   {
#ifndef IS_TESTNET
      FC_ASSERT( db().head_block_time() - account.last_owner_update > MUSE_OWNER_UPDATE_LIMIT );
#endif

      db().update_owner_authority( account, *o.owner );
   }

   db().modify( account, [this,&o]( account_object& acc )
   {
      if( o.active ) acc.active = *o.active;
      if( o.basic ) acc.basic = *o.basic;

      if( o.memo_key != public_key_type() )
            acc.memo_key = o.memo_key;

      if( ( o.active || o.owner ) && acc.active_challenged )
      {
         acc.active_challenged = false;
         acc.last_active_proved = db().head_block_time();
      }

#ifndef IS_LOW_MEM
      if ( o.json_metadata.size() > 0 )
         acc.json_metadata = o.json_metadata;
#endif
   });

}

void escrow_transfer_evaluator::do_apply( const escrow_transfer_operation& o ) {
try {
   FC_ASSERT( false, "Escrow transfer operation not enabled" ); // TODO: move to validate after HF

   const auto& from_account = db().get_account(o.from);
   db().get_account(o.to);
   const auto& agent_account = db().get_account(o.agent);

   FC_ASSERT( db().get_balance( from_account, o.amount.asset_id ) >= (o.amount + o.fee) );

   if( o.fee.amount > 0 ) {
      db().adjust_balance( from_account, -o.fee );
      db().adjust_balance( agent_account, o.fee );
   }

   db().adjust_balance( from_account, -o.amount );

   db().create<escrow_object>([&]( escrow_object& esc ) {
      esc.escrow_id  = o.escrow_id;
      esc.from       = o.from;
      esc.to         = o.to;
      esc.agent      = o.agent;
      esc.balance    = o.amount;
      esc.expiration = o.expiration;
   });

} FC_CAPTURE_AND_RETHROW( (o) ) }

void escrow_dispute_evaluator::do_apply( const escrow_dispute_operation& o ) {
try {
   FC_ASSERT( false, "Escrow dispute operation not enabled" ); // TODO: move to validate after HF
   db().get_account(o.from); // check if it exists

   const auto& e = db().get_escrow( o.from, o.escrow_id );
   FC_ASSERT( !e.disputed );
   FC_ASSERT( e.to == o.to );

   db().modify( e, [&]( escrow_object& esc ){
     esc.disputed = true;
   });
} FC_CAPTURE_AND_RETHROW( (o) ) }

void escrow_release_evaluator::do_apply( const escrow_release_operation& o ) {
try {
   FC_ASSERT( false, "Escrow release operation not enabled" ); // TODO: move to validate after HF
   db().get_account(o.from); // check if it exists
   const auto& to_account = db().get_account(o.to);
   db().get_account(o.who); // check if it exists

   const auto& e = db().get_escrow( o.from, o.escrow_id );
   FC_ASSERT( e.balance >= o.amount && e.balance.asset_id == o.amount.asset_id );
   /// TODO assert o.amount > 0

   if( e.expiration > db().head_block_time() ) {
      if( o.who == e.from )    FC_ASSERT( o.to == e.to );
      else if( o.who == e.to ) FC_ASSERT( o.to == e.from );
      else {
         FC_ASSERT( e.disputed && o.who == e.agent );
      }
   } else {
      FC_ASSERT( o.who == e.to || o.who == e.from );
   }

   db().adjust_balance( to_account, o.amount );
   if( e.balance == o.amount )
      db().remove( e );
   else {
      db().modify( e, [&]( escrow_object& esc ) {
         esc.balance -= o.amount;
      });
   }
} FC_CAPTURE_AND_RETHROW( (o) ) }

void transfer_evaluator::do_apply( const transfer_operation& o )
{
   const auto& from_account = db().get_account(o.from);
   const auto& to_account = db().get_account(o.to);

   if( from_account.active_challenged )
   {
      db().modify( from_account, [&]( account_object& a )
      {
         a.active_challenged = false;
         a.last_active_proved = db().head_block_time();
      });
   }

   if( o.amount.asset_id != VESTS_SYMBOL ) {
      FC_ASSERT( db().get_balance( from_account, o.amount.asset_id ) >= o.amount );
      db().adjust_balance( from_account, -o.amount );
      db().adjust_balance( to_account, o.amount );

   } else {
      FC_ASSERT( false , "transferring of Vestings (VEST) is not allowed." );
   }
}

void transfer_to_vesting_evaluator::do_apply( const transfer_to_vesting_operation& o )
{
   const auto& from_account = db().get_account(o.from);
   const auto& to_account = o.to.size() ? db().get_account(o.to) : from_account;

   FC_ASSERT( db().get_balance( from_account, MUSE_SYMBOL) >= o.amount );
   db().adjust_balance( from_account, -o.amount );
   db().create_vesting( to_account, o.amount );
}

void withdraw_vesting_evaluator::do_apply( const withdraw_vesting_operation& o )
{
    const auto& account = db().get_account( o.account );

    const auto now = db().head_block_time();
    if( now > fc::time_point::now() - fc::seconds(15) // SOFT FORK
          || db().has_hardfork( MUSE_HARDFORK_0_4 ) ) // TODO: move to withdraw_vesting_operation::validate after hf
       FC_ASSERT( o.vesting_shares.amount >= 0, "Cannot withdraw a negative amount of VESTS!" );

    FC_ASSERT( account.vesting_shares >= asset( 0, VESTS_SYMBOL ) );
    FC_ASSERT( account.vesting_shares - account.delegated_vesting_shares >= o.vesting_shares, "Account does not have sufficient Steem Power for withdraw." );

    const auto& props = db().get_dynamic_global_properties();
    const witness_schedule_object& wso = db().get_witness_schedule_object();

    asset min_vests = wso.median_props.account_creation_fee * props.get_vesting_share_price();
    min_vests.amount.value *= 10;

    FC_ASSERT( account.vesting_shares > min_vests,
               "Account registered by another account requires 10x account creation fee worth of Vestings before it can power down" );

    if( o.vesting_shares.amount <= 0 ) {
       if( o.vesting_shares.amount == 0 ) // SOFT FORK, remove after HF 4
       FC_ASSERT( account.vesting_withdraw_rate.amount  != 0, "this operation would not change the vesting withdraw rate" );

       db().modify( account, []( account_object& a ) {
         a.vesting_withdraw_rate = asset( 0, VESTS_SYMBOL );
         a.next_vesting_withdrawal = time_point_sec::maximum();
         a.to_withdraw = 0;
         a.withdrawn = 0;
       });
    }
    else {
       db().modify( account, [&o,&now]( account_object& a ) {
         auto new_vesting_withdraw_rate = asset( o.vesting_shares.amount / MUSE_VESTING_WITHDRAW_INTERVALS, VESTS_SYMBOL );

         if( new_vesting_withdraw_rate.amount == 0 )
            new_vesting_withdraw_rate.amount = 1;

         FC_ASSERT( a.vesting_withdraw_rate != new_vesting_withdraw_rate, "this operation would not change the vesting withdraw rate" );

         a.vesting_withdraw_rate = new_vesting_withdraw_rate;

         a.next_vesting_withdrawal = now + fc::seconds(MUSE_VESTING_WITHDRAW_INTERVAL_SECONDS);
         a.to_withdraw = o.vesting_shares.amount;
         a.withdrawn = 0;
       });
    }
}

void set_withdraw_vesting_route_evaluator::do_apply( const set_withdraw_vesting_route_operation& o )
{
   try
   {

   const auto& from_account = db().get_account( o.from_account );
   const auto& to_account = db().get_account( o.to_account );
   const auto& wd_idx = db().get_index_type< withdraw_vesting_route_index >().indices().get< by_withdraw_route >();
   auto itr = wd_idx.find( boost::make_tuple( from_account.id, to_account.id ) );

   if( itr == wd_idx.end() )
   {
      FC_ASSERT( o.percent != 0, "Cannot create a 0% destination." );
      FC_ASSERT( from_account.withdraw_routes < MUSE_MAX_WITHDRAW_ROUTES );

      db().create< withdraw_vesting_route_object >( [&]( withdraw_vesting_route_object& wvdo )
      {
         wvdo.from_account = from_account.id;
         wvdo.to_account = to_account.id;
         wvdo.percent = o.percent;
         wvdo.auto_vest = o.auto_vest;
      });

      db().modify( from_account, []( account_object& a )
      {
         a.withdraw_routes++;
      });
   }
   else if( o.percent == 0 )
   {
      db().remove( *itr );

      db().modify( from_account, []( account_object& a )
      {
         a.withdraw_routes--;
      });
   }
   else
   {
      db().modify( *itr, [&]( withdraw_vesting_route_object& wvdo )
      {
         wvdo.from_account = from_account.id;
         wvdo.to_account = to_account.id;
         wvdo.percent = o.percent;
         wvdo.auto_vest = o.auto_vest;
      });
   }

   itr = wd_idx.upper_bound( boost::make_tuple( from_account.id, account_id_type() ) );
   uint16_t total_percent = 0;

   while( itr != wd_idx.end() && itr->from_account == from_account.id )
   {
      total_percent += itr->percent;
      ++itr;
   }

   FC_ASSERT( total_percent <= MUSE_100_PERCENT, "More than 100% of vesting allocated to destinations" );
   }
   FC_CAPTURE_AND_RETHROW()
}

void account_witness_proxy_evaluator::do_apply( const account_witness_proxy_operation& o )
{
   const auto& account = db().get_account( o.account );
   FC_ASSERT( account.proxy != o.proxy, "something must change" );

   /// remove all current votes
   std::array<share_type, MUSE_MAX_PROXY_RECURSION_DEPTH+1> delta;
   delta[0] = -account.vesting_shares.amount;
   for( int i = 0; i < MUSE_MAX_PROXY_RECURSION_DEPTH; ++i )
      delta[i+1] = -account.proxied_vsf_votes[i];
   db().adjust_proxied_witness_votes( account, delta );

   if( o.proxy.size() ) {
      const auto& new_proxy = db().get_account( o.proxy );
      flat_set<account_id_type> proxy_chain({account.get_id(), new_proxy.get_id()});
      proxy_chain.reserve( MUSE_MAX_PROXY_RECURSION_DEPTH + 1 );

      /// check for proxy loops and fail to update the proxy if it would create a loop
      auto* cprox = &new_proxy;
      while( cprox->proxy.size() != 0 ) {
         const auto& next_proxy = db().get_account( cprox->proxy );
         FC_ASSERT( proxy_chain.insert( next_proxy.get_id() ).second, "Attempt to create a proxy loop" );
         cprox = &next_proxy;
         FC_ASSERT( proxy_chain.size() <= MUSE_MAX_PROXY_RECURSION_DEPTH, "Proxy chain is too long" );
      }

      /// clear all individual vote records
      db().clear_witness_votes( account );
      db().clear_streaming_platform_votes( account );

      db().modify( account, [&]( account_object& a ) {
         a.proxy = o.proxy;
      });

      /// add all new votes
      for( int i = 0; i <= MUSE_MAX_PROXY_RECURSION_DEPTH; ++i )
         delta[i] = -delta[i];
      db().adjust_proxied_witness_votes( account, delta );
   } else { /// we are clearing the proxy which means we simply update the account
      db().modify( account, [&]( account_object& a ) {
          a.proxy = o.proxy;
      });
   }
}


void account_witness_vote_evaluator::do_apply( const account_witness_vote_operation& o )
{
   const auto& voter = db().get_account( o.account );
   FC_ASSERT( voter.proxy.size() == 0, "A proxy is currently set, please clear the proxy before voting for a witness" );

   const auto& witness = db().get_witness( o.witness );

   const auto& by_account_witness_idx = db().get_index_type< witness_vote_index >().indices().get< by_account_witness >();
   auto itr = by_account_witness_idx.find( boost::make_tuple( voter.get_id(), witness.get_id() ) );

   if( itr == by_account_witness_idx.end() ) {
      FC_ASSERT( o.approve, "vote doesn't exist, user must be indicate a desire to approve witness" );

      FC_ASSERT( voter.witnesses_voted_for < MUSE_MAX_ACCOUNT_WITNESS_VOTES, "account has voted for too many witnesses" );

      db().create<witness_vote_object>( [&]( witness_vote_object& v ) {
          v.witness = witness.id;
          v.account = voter.id;
      });

      db().adjust_witness_vote( witness, voter.witness_vote_weight() );

      db().modify( voter, [&]( account_object& a ) {
         a.witnesses_voted_for++;
      });

   } else {
      FC_ASSERT( !o.approve, "vote currently exists, user must be indicate a desire to reject witness" );
      db().adjust_witness_vote( witness, -voter.witness_vote_weight() );

      db().modify( voter, [&]( account_object& a ) {
         a.witnesses_voted_for--;
      });
      db().remove( *itr );
   }
}

void custom_evaluator::do_apply( const custom_operation& o ){}

void custom_json_evaluator::do_apply( const custom_json_operation& o )
{
   if ( o.json.size() > 0 ) // TODO: move to validate after HF
   {
      FC_ASSERT( fc::json::is_valid(o.json), "JSON data not valid JSON" );
   }

   for( const auto& auth : o.required_basic_auths )
   {
      const auto& acnt = db().get_account( auth );
      FC_ASSERT( !( acnt.owner_challenged || acnt.active_challenged ) );
   }
}

void feed_publish_evaluator::do_apply( const feed_publish_operation& o )
{
  const auto& witness = db().get_witness( o.publisher );
  db().modify( witness, [&]( witness_object& w ){
      w.mbd_exchange_rate = o.exchange_rate;
      w.last_mbd_exchange_update = db().head_block_time();
  });
}

void convert_evaluator::do_apply( const convert_operation& o )
{
  if( o.amount.asset_id == MUSE_SYMBOL )
     FC_ASSERT( db().has_hardfork( MUSE_HARDFORK_0_6 ), "XSD -> xUSD conversion only allowed after hardfork 6!" );

  const auto& owner = db().get_account( o.owner );
  FC_ASSERT( db().get_balance( owner, o.amount.asset_id ) >= o.amount );

  db().adjust_balance( owner, -o.amount );

  const auto& fhistory = db().get_feed_history();
  FC_ASSERT( !fhistory.effective_median_history.is_null() );

  if( o.amount.asset_id == MUSE_SYMBOL )
  {
     const asset amount_to_issue = o.amount * fhistory.effective_median_history;

     db().adjust_balance( owner, amount_to_issue );

     db().push_applied_operation( fill_convert_request_operation ( o.owner, o.requestid, o.amount, amount_to_issue ) );

     db().modify( db().get_dynamic_global_properties(),
                  [&o,&amount_to_issue,&fhistory]( dynamic_global_property_object& p )
     {
        p.current_supply -= o.amount;
        p.current_mbd_supply += amount_to_issue;
        p.virtual_supply -= o.amount;
        p.virtual_supply += amount_to_issue * fhistory.effective_median_history;
     } );
  }
  else
     db().create<convert_request_object>( [&]( convert_request_object& obj )
     {
        obj.owner           = o.owner;
        obj.requestid       = o.requestid;
        obj.amount          = o.amount;
        obj.conversion_date = db().head_block_time() + MUSE_CONVERSION_DELAY; // 1 week
     });

}

void limit_order_create_evaluator::do_apply( const limit_order_create_operation& o )
{
   FC_ASSERT( o.expiration > db().head_block_time() );

   const auto& owner = db().get_account( o.owner );

   FC_ASSERT( db().get_balance( owner, o.amount_to_sell.asset_id ) >= o.amount_to_sell );

   db().adjust_balance( owner, -o.amount_to_sell );

   const auto& order = db().create<limit_order_object>( [&]( limit_order_object& obj )
   {
       obj.created    = db().head_block_time();
       obj.seller     = o.owner;
       obj.orderid    = o.orderid;
       obj.for_sale   = o.amount_to_sell.amount;
       obj.sell_price = o.get_price();
       obj.expiration = o.expiration;
   });

   bool filled = db().apply_order( order );

   if( o.fill_or_kill ) FC_ASSERT( filled );
}

void limit_order_create2_evaluator::do_apply( const limit_order_create2_operation& o )
{
   FC_ASSERT( o.expiration > db().head_block_time() );

   const auto& owner = db().get_account( o.owner );

   FC_ASSERT( db().get_balance( owner, o.amount_to_sell.asset_id ) >= o.amount_to_sell );

   db().adjust_balance( owner, -o.amount_to_sell );

   const auto& order = db().create<limit_order_object>( [&]( limit_order_object& obj )
   {
       obj.created    = db().head_block_time();
       obj.seller     = o.owner;
       obj.orderid    = o.orderid;
       obj.for_sale   = o.amount_to_sell.amount;
       obj.sell_price = o.exchange_rate;
       obj.expiration = o.expiration;
   });

   bool filled = db().apply_order( order );

   if( o.fill_or_kill ) FC_ASSERT( filled );
}

void limit_order_cancel_evaluator::do_apply( const limit_order_cancel_operation& o )
{
   db().cancel_order( db().get_limit_order( o.owner, o.orderid ) );
}

void report_over_production_evaluator::do_apply( const report_over_production_operation& o )
{
   FC_ASSERT( !db().is_producing(), "this operation is currently disabled" );
   FC_ASSERT( false , "this operation is disabled" ); // TODO: move to validate after HF
}

void challenge_authority_evaluator::do_apply( const challenge_authority_operation& o )
{
   const auto& challenged = db().get_account( o.challenged );
   const auto& challenger = db().get_account( o.challenger );

   if( o.require_owner )
   {
      FC_ASSERT( false, "Challenging the owner key is not supported at this time" );
#if 0
      FC_ASSERT( challenger.balance >= MUSE_OWNER_CHALLENGE_FEE );
      FC_ASSERT( !challenged.owner_challenged );
      FC_ASSERT( db().head_block_time() - challenged.last_owner_proved > MUSE_OWNER_CHALLENGE_COOLDOWN );

      db().adjust_balance( challenger, - MUSE_OWNER_CHALLENGE_FEE );
      db().create_vesting( db().get_account( o.challenged ), MUSE_OWNER_CHALLENGE_FEE );

      db().modify( challenged, [&]( account_object& a )
      {
         a.owner_challenged = true;
      });
#endif
  }
  else
  {
      FC_ASSERT( challenger.balance >= MUSE_ACTIVE_CHALLENGE_FEE );
      FC_ASSERT( !( challenged.owner_challenged || challenged.active_challenged ) );
      FC_ASSERT( db().head_block_time() - challenged.last_active_proved > MUSE_ACTIVE_CHALLENGE_COOLDOWN );

      db().adjust_balance( challenger, - MUSE_ACTIVE_CHALLENGE_FEE );
      db().create_vesting( db().get_account( o.challenged ), MUSE_ACTIVE_CHALLENGE_FEE );

      db().modify( challenged, [&]( account_object& a )
      {
         a.active_challenged = true;
      });
  }
}

void prove_authority_evaluator::do_apply( const prove_authority_operation& o )
{
   const auto& challenged = db().get_account( o.challenged );
   FC_ASSERT( challenged.owner_challenged || challenged.active_challenged );

   db().modify( challenged, [&]( account_object& a )
   {
      a.active_challenged = false;
      a.last_active_proved = db().head_block_time();
      if( o.require_owner )
      {
         a.owner_challenged = false;
         a.last_owner_proved = db().head_block_time();
      }
   });
}

void request_account_recovery_evaluator::do_apply( const request_account_recovery_operation& o )
{

   const auto& account_to_recover = db().get_account( o.account_to_recover );

   if ( account_to_recover.recovery_account.length() )   // Make sure recovery matches expected recovery account
      FC_ASSERT( account_to_recover.recovery_account == o.recovery_account );
   else                                                  // Empty string recovery account defaults to top witness
      FC_ASSERT( db().get_index_type< witness_index >().indices().get< by_vote_name >().begin()->owner == o.recovery_account );

   const auto& recovery_request_idx = db().get_index_type< account_recovery_request_index >().indices().get< by_account >();
   auto request = recovery_request_idx.find( o.account_to_recover );

   if( request == recovery_request_idx.end() ) // New Request
   {
      FC_ASSERT( !o.new_owner_authority.is_impossible(), "Cannot recover with an impossible authority" );
      FC_ASSERT( o.new_owner_authority.weight_threshold, "Cannot recover with an open authority" );

      // Check accounts in the new authority exist
      db().create< account_recovery_request_object >( [&]( account_recovery_request_object& req )
      {
         req.account_to_recover = o.account_to_recover;
         req.new_owner_authority = o.new_owner_authority;
         req.expires = db().head_block_time() + MUSE_ACCOUNT_RECOVERY_REQUEST_EXPIRATION_PERIOD;
      });
   }
   else if( o.new_owner_authority.weight_threshold == 0 ) // Cancel Request if authority is open
   {
      db().remove( *request );
   }
   else // Change Request
   {
      FC_ASSERT( !o.new_owner_authority.is_impossible(), "Cannot recover with an impossible authority" );

      db().modify( *request, [&]( account_recovery_request_object& req )
      {
         req.new_owner_authority = o.new_owner_authority;
         req.expires = db().head_block_time() + MUSE_ACCOUNT_RECOVERY_REQUEST_EXPIRATION_PERIOD;
      });
   }
}

void recover_account_evaluator::do_apply( const recover_account_operation& o )
{

   const auto& account = db().get_account( o.account_to_recover );

   FC_ASSERT( db().head_block_time() - account.last_account_recovery > MUSE_OWNER_UPDATE_LIMIT );

   const auto& recovery_request_idx = db().get_index_type< account_recovery_request_index >().indices().get< by_account >();
   auto request = recovery_request_idx.find( o.account_to_recover );

   FC_ASSERT( request != recovery_request_idx.end() );
   FC_ASSERT( request->new_owner_authority == o.new_owner_authority );

   const auto& recent_auth_idx = db().get_index_type< owner_authority_history_index >().indices().get< by_account >();
   auto hist = recent_auth_idx.lower_bound( o.account_to_recover );
   bool found = false;

   while( hist != recent_auth_idx.end() && hist->account == o.account_to_recover && !found )
   {
      found = hist->previous_owner_authority == o.recent_owner_authority;
      if( found ) break;
      ++hist;
   }

   FC_ASSERT( found, "Recent authority not found in authority history" );

   db().remove( *request ); // Remove first, update_owner_authority may invalidate iterator
   db().update_owner_authority( account, o.new_owner_authority );
   db().modify( account, [&]( account_object& a )
   {
      a.last_account_recovery = db().head_block_time();
   });
}

void change_recovery_account_evaluator::do_apply( const change_recovery_account_operation& o )
{

   db().get_account( o.new_recovery_account ); // Simply validate account exists
   const auto& account_to_recover = db().get_account( o.account_to_recover );

   const auto& change_recovery_idx = db().get_index_type< change_recovery_account_request_index >().indices().get< by_account >();
   auto request = change_recovery_idx.find( o.account_to_recover );

   if( request == change_recovery_idx.end() ) // New request
   {
      db().create< change_recovery_account_request_object >( [&]( change_recovery_account_request_object& req )
      {
         req.account_to_recover = o.account_to_recover;
         req.recovery_account = o.new_recovery_account;
         req.effective_on = db().head_block_time() + MUSE_OWNER_AUTH_RECOVERY_PERIOD;
      });
   }
   else if( account_to_recover.recovery_account != o.new_recovery_account ) // Change existing request
   {
      db().modify( *request, [&]( change_recovery_account_request_object& req )
      {
         req.recovery_account = o.new_recovery_account;
         req.effective_on = db().head_block_time() + MUSE_OWNER_AUTH_RECOVERY_PERIOD;
      });
   }
   else // Request exists and changing back to current recovery account
   {
      db().remove( *request );
   }
}

void delegate_vesting_shares_evaluator::do_apply( const delegate_vesting_shares_operation& op )
{
   auto& _db = db();

   FC_ASSERT( _db.has_hardfork( MUSE_HARDFORK_0_4 ), "Vesting delegation is only allowed after hardfork 0.4" );

   const auto& delegator = _db.get_account( op.delegator );
   const auto& delegatee = _db.get_account( op.delegatee );
   const streaming_platform_object* delegator_sp = _db.find_streaming_platform( op.delegator );
   const streaming_platform_object* delegatee_sp = _db.find_streaming_platform( op.delegatee );
   const auto& delegation_idx = _db.get_index_type< vesting_delegation_index >().indices().get< by_delegation >();
   auto delegation = delegation_idx.find( boost::make_tuple( op.delegator, op.delegatee ) );

   auto available_shares = delegator.vesting_shares - delegator.delegated_vesting_shares - asset( delegator.to_withdraw - delegator.withdrawn, VESTS_SYMBOL );

   const auto& wso = _db.get_witness_schedule_object();
   const auto& gpo = _db.get_dynamic_global_properties();

   auto min_delegation = asset( wso.median_props.account_creation_fee.amount * 10, MUSE_SYMBOL ) * gpo.get_vesting_share_price();
   auto min_update = wso.median_props.account_creation_fee * gpo.get_vesting_share_price();

   int64_t old_delegation = 0;
   share_type sp_delta = 0;
   // If delegation doesn't exist, create it
   if( delegation == delegation_idx.end() )
   {
      FC_ASSERT( available_shares >= op.vesting_shares, "Account does not have enough vesting shares to delegate." );
      FC_ASSERT( op.vesting_shares >= min_delegation, "Account must delegate a minimum of ${v}", ("v", min_delegation) );

      _db.create< vesting_delegation_object >( [&op,&_db]( vesting_delegation_object& obj )
      {
         obj.delegator = op.delegator;
         obj.delegatee = op.delegatee;
         obj.vesting_shares = op.vesting_shares;
         obj.min_delegation_time = _db.head_block_time();
      });

      _db.modify( delegator, [&op]( account_object& a )
      {
         a.delegated_vesting_shares += op.vesting_shares;
      });

      _db.modify( delegatee, [&op]( account_object& a )
      {
         a.received_vesting_shares += op.vesting_shares;
      });
      if( delegator_sp == nullptr && delegatee_sp != nullptr )
         sp_delta = op.vesting_shares.amount;
      else if( delegator_sp != nullptr && delegatee_sp == nullptr )
         sp_delta = -op.vesting_shares.amount;
   }
   // Else if the delegation is increasing
   else if( op.vesting_shares >= delegation->vesting_shares )
   {
      old_delegation = delegation->vesting_shares.amount.value;
      auto delta = op.vesting_shares - delegation->vesting_shares;

      FC_ASSERT( delta >= min_update,
                 "Vests increase is not enough of a difference. min_update: ${min}", ("min", min_update) );
      FC_ASSERT( available_shares >= op.vesting_shares - delegation->vesting_shares,
                 "Account does not have enough vesting shares to delegate." );

      _db.modify( delegator, [delta]( account_object& a )
      {
         a.delegated_vesting_shares += delta;
      });

      _db.modify( delegatee, [delta]( account_object& a )
      {
         a.received_vesting_shares += delta;
      });

      _db.modify( *delegation, [&op]( vesting_delegation_object& obj )
      {
         obj.vesting_shares = op.vesting_shares;
      });
      if( delegator_sp == nullptr && delegatee_sp != nullptr )
         sp_delta = delta.amount;
      else if( delegator_sp != nullptr && delegatee_sp == nullptr )
         sp_delta = -delta.amount;
   }
   // Else the delegation is decreasing
   else /* delegation->vesting_shares > op.vesting_shares */
   {
      old_delegation = delegation->vesting_shares.amount.value;
      auto delta = delegation->vesting_shares - op.vesting_shares;

      if( op.vesting_shares.amount > 0 )
      {
         FC_ASSERT( delta >= min_update, "Vests decrease is not enough of a difference. min_update: ${min}",
                    ("min", min_update) );
         FC_ASSERT( op.vesting_shares >= min_delegation,
                    "Delegation must be removed or leave minimum delegation amount of ${v}", ("v", min_delegation) );
      }
      else
      {
         FC_ASSERT( delegation->vesting_shares.amount > 0,
                    "Delegation would set vesting_shares to zero, but it is already zero" );
      }

      _db.create< vesting_delegation_expiration_object >( [&_db,&op,&gpo,&delegation,delta]( vesting_delegation_expiration_object& obj )
      {
         obj.delegator = op.delegator;
         obj.vesting_shares = delta;
         obj.expiration = std::max( _db.head_block_time() + gpo.delegation_return_period, delegation->min_delegation_time );
      });

      _db.modify( delegatee, [delta]( account_object& a )
      {
         a.received_vesting_shares -= delta;
      });

      if( op.vesting_shares.amount > 0 )
      {
         _db.modify( *delegation, [&op]( vesting_delegation_object& obj )
         {
            obj.vesting_shares = op.vesting_shares;
         });
      }
      else
      {
         _db.remove( *delegation );
      }
      if( delegator_sp == nullptr && delegatee_sp != nullptr )
         sp_delta = -delta.amount;
      // else if( delegator_sp != nullptr && delegatee_sp == nullptr )
         // delegator receives delegation back with a delay
   }

   if( sp_delta != 0 )
      _db.modify( gpo, [sp_delta] ( dynamic_global_property_object& dgpo ) {
         dgpo.total_vested_by_platforms += sp_delta;
      });

   if( old_delegation != op.vesting_shares.amount && !delegatee.redelegations.empty() )
   {
       map<account_id_type,int64_t> deltas;
       _db.modify( delegatee, [old_delegation,&deltas,&op] ( account_object& acct ) {
          for( auto& r : acct.redelegations )
          {
             const uint64_t old = ( fc::uint128_t( old_delegation )
                                    * r.second.redelegate_pct / MUSE_100_PERCENT ).to_uint64();
             const uint64_t now = ( fc::uint128_t( op.vesting_shares.amount.value )
                                    * r.second.redelegate_pct / MUSE_100_PERCENT ).to_uint64();
             const int64_t delta = static_cast<int64_t>( now ) - old;
             if( delta != 0 )
             {
                r.second.redelegated += delta;
                acct.redelegated_vesting_shares.amount += delta;
                deltas[r.first] = delta;
             }
          }
       });
       for( const auto& d : deltas )
       {
          const auto& acct = _db.get<account_object>( d.first );
          _db.modify( acct, [&d] ( account_object& acct ) {
              acct.rereceived_vesting_shares.amount += d.second;
          });
       }
   }
}

} } // muse::chain
