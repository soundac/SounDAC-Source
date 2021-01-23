#pragma once
#include <muse/chain/evaluator.hpp>
#include <muse/chain/protocol/base_operations.hpp>
#include <muse/chain/protocol/muse_operations.hpp>
#include <muse/chain/protocol/asset_ops.hpp>
#include <muse/chain/protocol/proposal.hpp>

namespace muse{ namespace chain {

#define DEFINE_EVALUATOR( X ) \
class X ## _evaluator : public evaluator< X ## _evaluator > { public: \
   typedef X ## _operation operation_type; \
   void do_evaluate( const X ## _operation& o ){};  \
   void do_apply( const X ## _operation& o ); \
}; 

DEFINE_EVALUATOR( account_create )
DEFINE_EVALUATOR( account_create_with_delegation )
DEFINE_EVALUATOR( account_update )
DEFINE_EVALUATOR( transfer )
DEFINE_EVALUATOR( transfer_to_vesting )
DEFINE_EVALUATOR( witness_update )
DEFINE_EVALUATOR( account_witness_vote )
DEFINE_EVALUATOR( account_witness_proxy )
DEFINE_EVALUATOR( asset_create )
DEFINE_EVALUATOR( asset_issue )
DEFINE_EVALUATOR( asset_update )
DEFINE_EVALUATOR( asset_reserve )
DEFINE_EVALUATOR( streaming_platform_update )
DEFINE_EVALUATOR( account_streaming_platform_vote )
DEFINE_EVALUATOR( streaming_platform_report )
DEFINE_EVALUATOR( withdraw_vesting )
DEFINE_EVALUATOR( set_withdraw_vesting_route )
DEFINE_EVALUATOR( content )
DEFINE_EVALUATOR( content_update )
DEFINE_EVALUATOR( content_disable )
DEFINE_EVALUATOR( content_approve )
DEFINE_EVALUATOR( vote )
DEFINE_EVALUATOR( custom )
DEFINE_EVALUATOR( custom_json )
DEFINE_EVALUATOR( feed_publish )
DEFINE_EVALUATOR( convert )
DEFINE_EVALUATOR( limit_order_create )
DEFINE_EVALUATOR( limit_order_cancel )
DEFINE_EVALUATOR( report_over_production )
DEFINE_EVALUATOR( limit_order_create2 )
DEFINE_EVALUATOR( escrow_transfer )
DEFINE_EVALUATOR( escrow_dispute )
DEFINE_EVALUATOR( escrow_release )
DEFINE_EVALUATOR( challenge_authority )
DEFINE_EVALUATOR( prove_authority )
DEFINE_EVALUATOR( request_account_recovery )
DEFINE_EVALUATOR( recover_account )
DEFINE_EVALUATOR( change_recovery_account )
DEFINE_EVALUATOR( proposal_create )
DEFINE_EVALUATOR( proposal_update )
DEFINE_EVALUATOR( proposal_delete )
DEFINE_EVALUATOR( friendship )
DEFINE_EVALUATOR( unfriend )
DEFINE_EVALUATOR( balance_claim )
DEFINE_EVALUATOR( delegate_vesting_shares )
DEFINE_EVALUATOR( request_stream_reporting )
DEFINE_EVALUATOR( cancel_stream_reporting )
} } // muse::chain
