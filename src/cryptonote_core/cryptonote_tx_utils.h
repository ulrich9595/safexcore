// Copyright (c) 2018, The Safex Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers
// Parts of this file are originally copyright (c) 2014-2018 The Monero Project

#pragma once
#include "cryptonote_basic/cryptonote_format_utils.h"
#include <boost/serialization/vector.hpp>
#include <boost/serialization/utility.hpp>
#include "ringct/rctOps.h"
#include "safex/safex_core.h"
#include "safex/safex_account.h"

namespace cryptonote
{
  //---------------------------------------------------------------
  bool construct_miner_tx(size_t height, size_t median_size, uint64_t already_generated_coins, size_t current_block_size, uint64_t fee, const account_public_address &miner_address, transaction& tx, const blobdata& extra_nonce = blobdata(), size_t max_outs = 999, uint8_t hard_fork_version = 1);

  bool generate_migration_key_image(const crypto::hash &bitcoin_transaction_hash, crypto::key_image &key_image);

  bool get_migration_verification_public_key(cryptonote::network_type nettype, crypto::public_key &public_key);


  struct tx_source_entry
  {
    typedef std::pair<uint64_t, rct::ctkey> output_entry;

    std::vector<output_entry> outputs;  //index + key
    size_t real_output = 0;             //index in outputs vector of real output_entry
    crypto::public_key real_out_tx_key = AUTO_VAL_INIT(real_out_tx_key); //incoming real tx public key
    std::vector<crypto::public_key> real_out_additional_tx_keys; //incoming real tx additional public keys
    size_t real_output_in_tx_index = 0;  //index in transaction outputs vector
    uint64_t amount = 0;                //cash
    uint64_t token_amount = 0;          //tokens
    cryptonote::tx_out_type referenced_output_type = tx_out_type::out_cash;
    safex::command_t command_type = safex::command_t::nop;
    cryptonote::blobdata command_safex_data;


    void push_output(uint64_t idx, const crypto::public_key &k, uint64_t amount) { outputs.push_back(std::make_pair(idx, rct::ctkey({rct::pk2rct(k), rct::zeroCommit(amount)}))); }

    BEGIN_SERIALIZE_OBJECT()
      FIELD(outputs)
      FIELD(real_output)
      FIELD(real_out_tx_key)
      FIELD(real_out_additional_tx_keys)
      FIELD(real_output_in_tx_index)
      FIELD(amount)
      FIELD(token_amount)
      FIELD(referenced_output_type)
      FIELD(command_type)
      FIELD(command_safex_data)


      if (real_output >= outputs.size())
        return false;
    END_SERIALIZE()
  };

  struct tx_destination_entry
  {
    uint64_t amount;                    //safex cash money
    uint64_t token_amount;              //amount of safex tokens
    account_public_address addr;        //destination address
    bool is_subaddress;
    bool token_transaction;             //output is safex tokens, not safex cash
    bool script_output;                 // if this is advanced output
    tx_out_type output_type;            //type of the output
    cryptonote::blobdata output_data;   //output safex data

    tx_destination_entry() : amount(0), token_amount(0), addr(AUTO_VAL_INIT(addr)), is_subaddress(false),
    token_transaction(false), script_output(false), output_type{tx_out_type::out_cash} {

    }



    tx_destination_entry(uint64_t a, const account_public_address &ad, bool is_subaddress, tx_out_type _out_type = tx_out_type::out_cash, cryptonote::blobdata _output_data={}) :
    amount(0), token_amount(0), addr(ad), is_subaddress(is_subaddress), token_transaction(is_token_output(_out_type)), script_output(is_script_output(_out_type)),
    output_type(_out_type), output_data{_output_data}
    {
      if ((_out_type == tx_out_type::out_token)
          || (_out_type == tx_out_type::out_staked_token))
      {
        token_amount = a;
      } else {
        amount = a;
      }

    }

    constexpr bool is_token_output(tx_out_type _out_type) const { return _out_type == tx_out_type::out_token;}
    constexpr bool is_cash_output(tx_out_type _out_type) const { return _out_type == tx_out_type::out_cash;}
    constexpr bool is_script_output(tx_out_type _out_type) const { return (_out_type >= tx_out_type::out_advanced && _out_type < tx_out_type::out_invalid );}


    BEGIN_SERIALIZE_OBJECT()
      VARINT_FIELD(amount)
      VARINT_FIELD(token_amount)
      FIELD(addr)
      FIELD(is_subaddress)
      FIELD(token_transaction)
      FIELD(script_output)
      FIELD(output_type)
    END_SERIALIZE()
  };

  //---------------------------------------------------------------
  crypto::public_key get_destination_view_key_pub(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr);
  bool construct_tx(const account_keys& sender_account_keys, std::vector<tx_source_entry> &sources, const std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, std::vector<uint8_t> extra, transaction& tx, uint64_t unlock_time, const safex::safex_account_keys &sfx_account_keys={});
  bool construct_tx_with_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, std::vector<uint8_t> extra, transaction& tx, uint64_t unlock_time, const crypto::secret_key &tx_key, const std::vector<crypto::secret_key> &additional_tx_keys, bool shuffle_outs = true);
  bool construct_advanced_tx_with_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, std::vector<uint8_t> extra, transaction& tx, uint64_t unlock_time, const crypto::secret_key &tx_key, const std::vector<crypto::secret_key> &additional_tx_keys, const safex::safex_account_keys &sfx_acc_keys, bool shuffle_outs = true);
  bool construct_tx_and_get_tx_key(const account_keys& sender_account_keys, const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses, std::vector<tx_source_entry>& sources, std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, std::vector<uint8_t> extra, transaction& tx, uint64_t unlock_time, crypto::secret_key &tx_key, std::vector<crypto::secret_key> &additional_tx_keys, const safex::safex_account_keys &sfx_acc_keys = safex::safex_account_keys{});


  inline bool is_advanced_transaction(const std::vector<tx_source_entry>& sources);

  bool generate_genesis_block(
      block& bl
    , std::string const & genesis_tx
    , uint32_t nonce
    );

  cryptonote::tx_source_entry::output_entry generate_migration_bitcoin_transaction_output(const account_keys& sender_account_keys, const crypto::hash bitcoin_tx_hash, uint64_t token_amount);

  namespace fakechain
  {
    void set_core_tests_public_key(const crypto::public_key& publicKey);
  }

}

BOOST_CLASS_VERSION(cryptonote::tx_source_entry, 0)
BOOST_CLASS_VERSION(cryptonote::tx_destination_entry, 0)

namespace boost
{
  namespace serialization
  {
    template <class Archive>
    inline void serialize(Archive &a, cryptonote::tx_source_entry &x, const boost::serialization::version_type ver)
    {
      a & x.outputs;
      a & x.real_output;
      a & x.real_out_tx_key;
      a & x.real_output_in_tx_index;
      a & x.real_out_additional_tx_keys;
      a & x.amount;
      a & x.token_amount;
      a & x.referenced_output_type;
      a & x.command_type;

    }

    template <class Archive>
    inline void serialize(Archive& a, cryptonote::tx_destination_entry& x, const boost::serialization::version_type ver)
    {
      a & x.amount;
      a & x.addr;
      a & x.is_subaddress;
      a & x.token_amount;
      a & x.token_transaction;
      a & x.script_output;
      a & x.output_type;
    }
  }
}
