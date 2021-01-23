/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
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
#pragma once
#include <muse/chain/config.hpp>
#include <muse/chain/protocol/address.hpp>
#include <muse/chain/protocol/types.hpp>
//#include <muse/chain/immutable_chain_parameters.hpp>

#include <fc/crypto/sha256.hpp>

#include <string>
#include <vector>

namespace muse { namespace chain {
using std::string;
using std::vector;

struct genesis_state_type {
   struct initial_account_type {
      initial_account_type(const string& name = string(),
                           const public_key_type& owner_key = public_key_type(),
                           const public_key_type& active_key = public_key_type())
         : name(name),
           owner_key(owner_key),
           active_key(active_key == public_key_type()? owner_key : active_key)
      {}
      string name;
      public_key_type owner_key;
      public_key_type active_key;
   };
   struct initial_asset_type {

      string symbol;
      string issuer_name;

      string description;
      uint8_t precision = MUSE_ASSET_PRECISION;

      share_type max_supply;

   };
   struct initial_balance_type {
      address owner;
      string asset_symbol;
      share_type amount;
   };

   struct initial_vesting_balance_type {
      account_id_type owner;
      string asset_symbol;
      share_type amount;
      time_point_sec begin_timestamp;
      uint32_t vesting_duration_seconds = 0;
      share_type begin_balance;
   };
   struct initial_witness_type {
      /// Must correspond to one of the initial accounts
      string owner_name;
      public_key_type block_signing_key;
   };

   time_point_sec                           initial_timestamp;
   share_type                               max_core_supply = MUSE_MAX_SHARE_SUPPLY;
   share_type                               init_supply = 0; // not reflected, only used in tests
   vector<initial_account_type>             initial_accounts;
   vector<initial_asset_type>               initial_assets;
   vector<initial_balance_type>             initial_balances;
   vector<initial_vesting_balance_type>     initial_vesting_balances;
   uint64_t                                 initial_active_witnesses = 1;
   vector<initial_witness_type>             initial_witness_candidates;
   chain_id_type                            initial_chain_id; // not reflected, computed from file
   fc::sha256                               json_hash; // not reflected, computed from file
   /**
    * Get the chain_id corresponding to this genesis state.
    *
    * This is the SHA256 serialization of the genesis_state.
    */
};

} } // namespace muse::chain

FC_REFLECT(muse::chain::genesis_state_type::initial_account_type, (name)(owner_key)(active_key))

FC_REFLECT(muse::chain::genesis_state_type::initial_asset_type,
           (symbol)(issuer_name)(description)(precision)(max_supply))

FC_REFLECT(muse::chain::genesis_state_type::initial_balance_type,
           (owner)(asset_symbol)(amount))

FC_REFLECT(muse::chain::genesis_state_type::initial_vesting_balance_type,
           (owner)(asset_symbol)(amount)(begin_timestamp)(vesting_duration_seconds)(begin_balance))

FC_REFLECT(muse::chain::genesis_state_type::initial_witness_type, (owner_name)(block_signing_key))

FC_REFLECT(muse::chain::genesis_state_type,
           (initial_timestamp)(max_core_supply)(initial_accounts)(initial_assets)(initial_balances)
           (initial_vesting_balances)(initial_active_witnesses)(initial_witness_candidates)
           )
