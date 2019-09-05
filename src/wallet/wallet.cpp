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
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO gEVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers
// Parts of this file are originally copyright (c) 2014-2018 The Monero Project

#include <numeric>
#include <iostream>
#include <random>
#include <tuple>
#include <boost/format.hpp>
#include <boost/optional/optional.hpp>
#include <boost/utility/value_init.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/variant/apply_visitor.hpp>
#include "include_base_utils.h"
using namespace epee;

#include "cryptonote_config.h"
#include "wallet.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "rpc/core_rpc_server_commands_defs.h"
#include "misc_language.h"
#include "cryptonote_basic/cryptonote_basic_impl.h"
#include "common/boost_serialization_helper.h"
#include "common/command_line.h"
#include "common/threadpool.h"
#include "profile_tools.h"
#include "crypto/crypto.h"
#include "serialization/binary_utils.h"
#include "serialization/string.h"
#include "cryptonote_basic/blobdatatype.h"
#include "mnemonics/electrum-words.h"
#include "common/i18n.h"
#include "common/util.h"
#include "common/apply_permutation.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "common/json_util.h"
#include "memwipe.h"
#include "common/base58.h"
#include "common/dns_utils.h"
#include "ringdb.h"

#include "safex/command.h"

extern "C"
{
#include "crypto/keccak.h"
#include "crypto/crypto-ops.h"
}
using namespace std;
using namespace crypto;
using namespace cryptonote;

#undef SAFEX_DEFAULT_LOG_CATEGORY
#define SAFEX_DEFAULT_LOG_CATEGORY "wallet.wallet"

// used to choose when to stop adding outputs to a tx
#define APPROXIMATE_INPUT_BYTES 80

// used to target a given block size (additional outputs may be added on top to build fee)
#define TX_SIZE_TARGET(bytes) (bytes*2/3)

// arbitrary, used to generate different hashes from the same input
#define CHACHA8_KEY_TAIL 0x8c

#define UNSIGNED_TX_PREFIX "Safex unsigned tx set\004"
#define SIGNED_TX_PREFIX "Safex signed tx set\004"


#define RECENT_OUTPUT_RATIO (0.5) // 50% of outputs are from the recent zone
#define RECENT_OUTPUT_DAYS (1.8) // last 1.8 day makes up the recent zone (taken from monerolink.pdf, Miller et al)
#define RECENT_OUTPUT_ZONE ((time_t)(RECENT_OUTPUT_DAYS * 86400))
#define RECENT_OUTPUT_BLOCKS (RECENT_OUTPUT_DAYS * 720)

#define FEE_ESTIMATE_GRACE_BLOCKS 10 // estimate fee valid for that many blocks

#define SECOND_OUTPUT_RELATEDNESS_THRESHOLD 0.0f

#define SUBADDRESS_LOOKAHEAD_MAJOR 50
#define SUBADDRESS_LOOKAHEAD_MINOR 200

#define KEY_IMAGE_EXPORT_FILE_MAGIC "Safex key image export\002"

#define SEGREGATION_FORK_HEIGHT 10000000
#define TESTNET_SEGREGATION_FORK_HEIGHT 10000000
#define STAGENET_SEGREGATION_FORK_HEIGHT 10000000
#define SEGREGATION_FORK_VICINITY 1500 /* blocks */


namespace
{
  std::string get_default_ringdb_path()
  {
    boost::filesystem::path dir = tools::get_default_data_dir();
    // remove .bitsafex, replace with .shared-ringdb
    dir = dir.remove_filename();
    dir /= ".shared-ringdb";
    return dir.string();
  }
}

namespace
{
// Create on-demand to prevent static initialization order fiasco issues.
struct options {
  const command_line::arg_descriptor<std::string> daemon_address = {"daemon-address", tools::wallet::tr("Use daemon instance at <host>:<port>"), ""};
  const command_line::arg_descriptor<std::string> daemon_host = {"daemon-host", tools::wallet::tr("Use daemon instance at host <arg> instead of localhost"), ""};
  const command_line::arg_descriptor<std::string> password = {"password", tools::wallet::tr("Wallet password (escape/quote as needed)"), "", true};
  const command_line::arg_descriptor<std::string> password_file = {"password-file", tools::wallet::tr("Wallet password file"), "", true};
  const command_line::arg_descriptor<int> daemon_port = {"daemon-port", tools::wallet::tr("Use daemon instance at port <arg> instead of 18081"), 0};
  const command_line::arg_descriptor<std::string> daemon_login = {"daemon-login", tools::wallet::tr("Specify username[:password] for daemon RPC client"), "", true};
  const command_line::arg_descriptor<bool> testnet = {"testnet", tools::wallet::tr("For testnet. Daemon must also be launched with --testnet flag"), false};
  const command_line::arg_descriptor<bool> stagenet = {"stagenet", tools::wallet::tr("For stagenet. Daemon must also be launched with --stagenet flag"), false};
  const command_line::arg_descriptor<bool> restricted = {"restricted-rpc", tools::wallet::tr("Restricts to view-only commands"), false};
  const command_line::arg_descriptor<std::string, false, true> shared_ringdb_dir = {
    "shared-ringdb-dir", tools::wallet::tr("Set shared ring database path"),
    get_default_ringdb_path(),
    testnet,
    [](bool testnet, bool defaulted, std::string val)->std::string {
      if (testnet)
        return (boost::filesystem::path(val) / "testnet").string();
      return val;
    }
  };
};

void do_prepare_file_names(const std::string& file_path, std::string& keys_file, std::string& wallet_file)
{
  keys_file = file_path;
  wallet_file = file_path;
  boost::system::error_code e;
  if(string_tools::get_extension(keys_file) == "keys")
  {//provided keys file name
    wallet_file = string_tools::cut_off_extension(wallet_file);
  }else
  {//provided wallet file name
    keys_file += ".keys";
  }
}

uint64_t calculate_fee(uint64_t fee_per_kb, size_t bytes, uint64_t fee_multiplier)
{
  uint64_t kB = (bytes + 1023) / 1024;
  return kB * fee_per_kb * fee_multiplier;
}

uint64_t calculate_fee(uint64_t fee_per_kb, const cryptonote::blobdata &blob, uint64_t fee_multiplier)
{
  return calculate_fee(fee_per_kb, blob.size(), fee_multiplier);
}

std::string get_size_string(size_t sz)
{
  return std::to_string(sz) + " bytes (" + std::to_string((sz + 1023) / 1024) + " kB)";
}

std::string get_size_string(const cryptonote::blobdata &tx)
{
  return get_size_string(tx.size());
}

std::unique_ptr<tools::wallet> make_basic(const boost::program_options::variables_map& vm, const options& opts, const std::function<boost::optional<tools::password_container>(const char *, bool)> &password_prompter)
{
  const bool testnet = command_line::get_arg(vm, opts.testnet);
  const bool stagenet = command_line::get_arg(vm, opts.stagenet);
  const bool restricted = command_line::get_arg(vm, opts.restricted);

  auto daemon_address = command_line::get_arg(vm, opts.daemon_address);
  auto daemon_host = command_line::get_arg(vm, opts.daemon_host);
  auto daemon_port = command_line::get_arg(vm, opts.daemon_port);

  THROW_WALLET_EXCEPTION_IF(!daemon_address.empty() && !daemon_host.empty() && 0 != daemon_port,
      tools::error::wallet_internal_error, tools::wallet::tr("can't specify daemon host or port more than once"));

  boost::optional<epee::net_utils::http::login> login{};
  if (command_line::has_arg(vm, opts.daemon_login))
  {
    auto parsed = tools::login::parse(
      command_line::get_arg(vm, opts.daemon_login), false, [password_prompter](bool verify) {
        return password_prompter("Daemon client password", verify);
      }
    );
    if (!parsed)
      return nullptr;

    login.emplace(std::move(parsed->username), std::move(parsed->password).password());
  }

  if (daemon_host.empty())
    daemon_host = "localhost";

  if (!daemon_port)
  {
    daemon_port = testnet ? config::testnet::RPC_DEFAULT_PORT : stagenet ? config::stagenet::RPC_DEFAULT_PORT : config::RPC_DEFAULT_PORT;
  }

  if (daemon_address.empty())
    daemon_address = std::string("http://") + daemon_host + ":" + std::to_string(daemon_port);

  std::unique_ptr<tools::wallet> wallet(new tools::wallet(testnet ? TESTNET : stagenet ? STAGENET : MAINNET, restricted));
  wallet->init(std::move(daemon_address), std::move(login));
  boost::filesystem::path ringdb_path = command_line::get_arg(vm, opts.shared_ringdb_dir);
  wallet->set_ring_database(ringdb_path.string());
  return wallet;
}

boost::optional<tools::password_container> get_password(const boost::program_options::variables_map& vm, const options& opts, const std::function<boost::optional<tools::password_container>(const char*, bool)> &password_prompter, const bool verify)
{
  if (command_line::has_arg(vm, opts.password) && command_line::has_arg(vm, opts.password_file))
  {
    THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("can't specify more than one of --password and --password-file"));
  }

  if (command_line::has_arg(vm, opts.password))
  {
    return tools::password_container{command_line::get_arg(vm, opts.password)};
  }

  if (command_line::has_arg(vm, opts.password_file))
  {
    std::string password;
    bool r = epee::file_io_utils::load_file_to_string(command_line::get_arg(vm, opts.password_file),
                                                      password);
    THROW_WALLET_EXCEPTION_IF(!r, tools::error::wallet_internal_error, tools::wallet::tr("the password file specified could not be read"));

    // Remove line breaks the user might have inserted
    boost::trim_right_if(password, boost::is_any_of("\r\n"));
    return {tools::password_container{std::move(password)}};
  }

  THROW_WALLET_EXCEPTION_IF(!password_prompter, tools::error::wallet_internal_error, tools::wallet::tr("no password specified; use --prompt-for-password to prompt for a password"));

  return password_prompter(verify ? tr("Enter a new password for the wallet") : tr("Wallet password"), verify);
}

std::unique_ptr<tools::wallet> generate_from_json(const std::string& json_file, const boost::program_options::variables_map& vm, const options& opts, const std::function<boost::optional<tools::password_container>(const char *, bool)> &password_prompter)
{
  const bool testnet = command_line::get_arg(vm, opts.testnet);
  const bool stagenet = command_line::get_arg(vm, opts.stagenet);
  const network_type nettype = testnet ? TESTNET : stagenet ? STAGENET : MAINNET;

  /* GET_FIELD_FROM_JSON_RETURN_ON_ERROR Is a generic macro that can return
  false. Gcc will coerce this into unique_ptr(nullptr), but clang correctly
  fails. This large wrapper is for the use of that macro */
  std::unique_ptr<tools::wallet> wallet;
  const auto do_generate = [&]() -> bool {
    std::string buf;
    if (!epee::file_io_utils::load_file_to_string(json_file, buf)) {
      THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, std::string(tools::wallet::tr("Failed to load file ")) + json_file);
      return false;
    }

    rapidjson::Document json;
    if (json.Parse(buf.c_str()).HasParseError()) {
      THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("Failed to parse JSON"));
      return false;
    }

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, version, unsigned, Uint, true, 0);
    const int current_version = 1;
    THROW_WALLET_EXCEPTION_IF(field_version > current_version, tools::error::wallet_internal_error,
      ((boost::format(tools::wallet::tr("Version %u too new, we can only grok up to %u")) % field_version % current_version)).str());

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, filename, std::string, String, true, std::string());

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, scan_from_height, uint64_t, Uint64, false, 0);
    const bool recover = field_scan_from_height_found;

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, password, std::string, String, false, std::string());

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, viewkey, std::string, String, false, std::string());
    crypto::secret_key viewkey;
    if (field_viewkey_found)
    {
      cryptonote::blobdata viewkey_data;
      if(!epee::string_tools::parse_hexstr_to_binbuff(field_viewkey, viewkey_data) || viewkey_data.size() != sizeof(crypto::secret_key))
      {
        THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("failed to parse view key secret key"));
      }
      viewkey = *reinterpret_cast<const crypto::secret_key*>(viewkey_data.data());
      crypto::public_key pkey;
      if (!crypto::secret_key_to_public_key(viewkey, pkey)) {
        THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("failed to verify view key secret key"));
      }
    }

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, spendkey, std::string, String, false, std::string());
    crypto::secret_key spendkey;
    if (field_spendkey_found)
    {
      cryptonote::blobdata spendkey_data;
      if(!epee::string_tools::parse_hexstr_to_binbuff(field_spendkey, spendkey_data) || spendkey_data.size() != sizeof(crypto::secret_key))
      {
        THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("failed to parse spend key secret key"));
      }
      spendkey = *reinterpret_cast<const crypto::secret_key*>(spendkey_data.data());
      crypto::public_key pkey;
      if (!crypto::secret_key_to_public_key(spendkey, pkey)) {
        THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("failed to verify spend key secret key"));
      }
    }

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, seed, std::string, String, false, std::string());
    std::string old_language;
    crypto::secret_key recovery_key;
    bool restore_deterministic_wallet = false;
    if (field_seed_found)
    {
      if (!crypto::ElectrumWords::words_to_bytes(field_seed, recovery_key, old_language))
      {
        THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("Electrum-style word list failed verification"));
      }
      restore_deterministic_wallet = true;

      GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, seed_passphrase, std::string, String, false, std::string());
      if (field_seed_passphrase_found)
      {
        if (!field_seed_passphrase.empty())
          recovery_key = cryptonote::decrypt_key(recovery_key, field_seed_passphrase);
      }
    }

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, address, std::string, String, false, std::string());

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, create_address_file, int, Int, false, false);
    bool create_address_file = field_create_address_file;

    // compatibility checks
    if (!field_seed_found && !field_viewkey_found && !field_spendkey_found)
    {
      THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("At least one of either an Electrum-style word list, private view key, or private spend key must be specified"));
    }
    if (field_seed_found && (field_viewkey_found || field_spendkey_found))
    {
      THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("Both Electrum-style word list and private key(s) specified"));
    }

    // if an address was given, we check keys against it, and deduce the spend
    // public key if it was not given
    if (field_address_found)
    {
      cryptonote::address_parse_info info;
      if(!get_account_address_from_str(info, nettype, field_address))
      {
        THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("invalid address"));
      }
      if (field_viewkey_found)
      {
        crypto::public_key pkey;
        if (!crypto::secret_key_to_public_key(viewkey, pkey)) {
          THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("failed to verify view key secret key"));
        }
        if (info.address.m_view_public_key != pkey) {
          THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("view key does not match standard address"));
        }
      }
      if (field_spendkey_found)
      {
        crypto::public_key pkey;
        if (!crypto::secret_key_to_public_key(spendkey, pkey)) {
          THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("failed to verify spend key secret key"));
        }
        if (info.address.m_spend_public_key != pkey) {
          THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("spend key does not match standard address"));
        }
      }
    }

    const bool deprecated_wallet = restore_deterministic_wallet && ((old_language == crypto::ElectrumWords::old_language_name) ||
      crypto::ElectrumWords::get_is_old_style_seed(field_seed));
    THROW_WALLET_EXCEPTION_IF(deprecated_wallet, tools::error::wallet_internal_error,
      tools::wallet::tr("Cannot generate deprecated wallets from JSON"));

    wallet.reset(make_basic(vm, opts, password_prompter).release());
    wallet->set_refresh_from_block_height(field_scan_from_height);
    wallet->explicit_refresh_from_block_height(field_scan_from_height_found);

    try
    {
      if (!field_seed.empty())
      {
        wallet->generate(field_filename, field_password, recovery_key, recover, false, create_address_file);
      }
      else if (field_viewkey.empty() && !field_spendkey.empty())
      {
        wallet->generate(field_filename, field_password, spendkey, recover, false, create_address_file);
      }
      else
      {
        cryptonote::account_public_address address;
        if (!crypto::secret_key_to_public_key(viewkey, address.m_view_public_key)) {
          THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("failed to verify view key secret key"));
        }

        if (field_spendkey.empty())
        {
          // if we have an address but no spend key, we can deduce the spend public key
          // from the address
          if (field_address_found)
          {
            cryptonote::address_parse_info info;
            if(!get_account_address_from_str(info, nettype, field_address))
            {
              THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, std::string(tools::wallet::tr("failed to parse address: ")) + field_address);
            }
            address.m_spend_public_key = info.address.m_spend_public_key;
          }
          else
          {
            THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("Address must be specified in order to create watch-only wallet"));
          }
          wallet->generate(field_filename, field_password, address, viewkey, create_address_file);
        }
        else
        {
          if (!crypto::secret_key_to_public_key(spendkey, address.m_spend_public_key)) {
            THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, tools::wallet::tr("failed to verify spend key secret key"));
          }
          wallet->generate(field_filename, field_password, address, spendkey, viewkey, create_address_file);
        }
      }
    }
    catch (const std::exception& e)
    {
      THROW_WALLET_EXCEPTION(tools::error::wallet_internal_error, std::string(tools::wallet::tr("failed to generate new wallet: ")) + e.what());
    }
    return true;
  };

  if (do_generate())
  {
    return wallet;
  }
  return nullptr;
}

static void throw_on_rpc_response_error(const boost::optional<std::string> &status, const char *method)
{
  // no error
  if (!status)
    return;

  // empty string -> not connection
  THROW_WALLET_EXCEPTION_IF(status->empty(), tools::error::no_connection_to_daemon, method);

  THROW_WALLET_EXCEPTION_IF(*status == CORE_RPC_STATUS_BUSY, tools::error::daemon_busy, method);
  THROW_WALLET_EXCEPTION_IF(*status != CORE_RPC_STATUS_OK, tools::error::wallet_generic_rpc_error, method, *status);
}

std::string strjoin(const std::vector<size_t> &V, const char *sep)
{
  std::stringstream ss;
  bool first = true;
  for (const auto &v: V)
  {
    if (!first)
      ss << sep;
    ss << std::to_string(v);
    first = false;
  }
  return ss.str();
}

static void emplace_or_replace(std::unordered_multimap<crypto::hash, tools::wallet::pool_payment_details> &container,
  const crypto::hash &key, const tools::wallet::pool_payment_details &pd)
{
  auto range = container.equal_range(key);
  for (auto i = range.first; i != range.second; ++i)
  {
    if (i->second.m_pd.m_tx_hash == pd.m_pd.m_tx_hash && i->second.m_pd.m_subaddr_index == pd.m_pd.m_subaddr_index)
    {
      i->second = pd;
      return;
    }
  }
  container.emplace(key, pd);
}

void drop_from_short_history(std::list<crypto::hash> &short_chain_history, size_t N)
{
  std::list<crypto::hash>::iterator right;
  // drop early N off, skipping the genesis block
  if (short_chain_history.size() > N) {
    right = short_chain_history.end();
    std::advance(right,-1);
    std::list<crypto::hash>::iterator left = right;
    std::advance(left, -N);
    short_chain_history.erase(left, right);
  }
}

size_t estimate_tx_size(int n_inputs, int mixin, int n_outputs, size_t extra_size)
{
    return n_inputs * (mixin+1) * APPROXIMATE_INPUT_BYTES + extra_size;
}

crypto::hash8 get_short_payment_id(const tools::wallet::pending_tx &ptx, hw::device &hwdev)
{
  crypto::hash8 payment_id8 = null_hash8;
  std::vector<tx_extra_field> tx_extra_fields;
  parse_tx_extra(ptx.tx.extra, tx_extra_fields); // ok if partially parsed
  cryptonote::tx_extra_nonce extra_nonce;
  if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
  {
    if(get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
    {
      if (ptx.dests.empty())
      {
        MWARNING("Encrypted payment id found, but no destinations public key, cannot decrypt");
        return crypto::null_hash8;
      }
      hwdev.decrypt_payment_id(payment_id8, ptx.dests[0].addr.m_view_public_key, ptx.tx_key);
    }
  }
  return payment_id8;
}

tools::wallet::tx_construction_data get_construction_data_with_decrypted_short_payment_id(const tools::wallet::pending_tx &ptx, hw::device &hwdev)
{
  tools::wallet::tx_construction_data construction_data = ptx.construction_data;
  crypto::hash8 payment_id = get_short_payment_id(ptx,hwdev);
  if (payment_id != null_hash8)
  {
    // Remove encrypted
    remove_field_from_tx_extra(construction_data.extra, typeid(cryptonote::tx_extra_nonce));
    // Add decrypted
    std::string extra_nonce;
    set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id);
    THROW_WALLET_EXCEPTION_IF(!add_extra_nonce_to_tx_extra(construction_data.extra, extra_nonce),
        tools::error::wallet_internal_error, "Failed to add decrypted payment id to tx extra");
    LOG_PRINT_L1("Decrypted payment ID: " << payment_id);
  }
  return construction_data;
}

uint32_t get_subaddress_clamped_sum(uint32_t idx, uint32_t extra)
{
  static constexpr uint32_t uint32_max = std::numeric_limits<uint32_t>::max();
  if (idx > uint32_max - extra)
    return uint32_max;
  return idx + extra;
}

  //-----------------------------------------------------------------
} //namespace

namespace tools
{
// for now, limit to 30 attempts.  TODO: discuss a good number to limit to.
const size_t MAX_SPLIT_ATTEMPTS = 10;

constexpr const std::chrono::seconds wallet::rpc_timeout;
const char* wallet::tr(const char* str) { return i18n_translate(str, "tools::wallet"); }

wallet::wallet(network_type nettype, bool restricted):
  m_multisig_rescan_info(NULL), //not used, kept for binary compatibility
  m_multisig_rescan_k(NULL), //not used, kept for binary compatibility
  m_run(true),
  m_callback(0),
  m_nettype(nettype),
  m_always_confirm_transfers(true),
  m_print_ring_members(false),
  m_store_tx_info(true),
  m_default_mixin(0),
  m_default_priority(0),
  m_refresh_type(RefreshOptimizeCoinbase),
  m_auto_refresh(true),
  m_refresh_from_block_height(0),
  m_explicit_refresh_from_block_height(true),
  m_confirm_missing_payment_id(true),
  m_confirm_non_default_ring_size(true),
  m_ask_password(true),
  m_min_output_count(0),
  m_min_output_value(0),
  m_merge_destinations(false),
  m_confirm_backlog(true),
  m_confirm_backlog_threshold(0),
  m_confirm_export_overwrite(true),
  m_auto_low_priority(true),
  m_segregate_pre_fork_outputs(true),
  m_key_reuse_mitigation2(true),
  m_segregation_height(0),
  m_is_initialized(false),
  m_restricted(restricted),
  is_old_file_format(false),
  m_node_rpc_proxy(m_http_client, m_daemon_rpc_mutex),
  m_subaddress_lookahead_major(SUBADDRESS_LOOKAHEAD_MAJOR),
  m_subaddress_lookahead_minor(SUBADDRESS_LOOKAHEAD_MINOR),
  m_light_wallet(false),
  m_light_wallet_scanned_block_height(0),
  m_light_wallet_blockchain_height(0),
  m_light_wallet_connected(false),
  m_light_wallet_balance(0),
  m_light_wallet_unlocked_balance(0),
  m_light_wallet_token_balance(0),
  m_light_wallet_unlocked_token_balance(0),
  m_key_on_device(false),
  m_ring_history_saved(false),
  m_ringdb()
{
}

wallet::~wallet()
{
}

bool wallet::has_testnet_option(const boost::program_options::variables_map& vm)
{
  return command_line::get_arg(vm, options().testnet);
}

bool wallet::has_stagenet_option(const boost::program_options::variables_map& vm)
{
  return command_line::get_arg(vm, options().stagenet);
}

void wallet::init_options(boost::program_options::options_description& desc_params)
{
  const options opts{};
  command_line::add_arg(desc_params, opts.daemon_address);
  command_line::add_arg(desc_params, opts.daemon_host);
  command_line::add_arg(desc_params, opts.password);
  command_line::add_arg(desc_params, opts.password_file);
  command_line::add_arg(desc_params, opts.daemon_port);
  command_line::add_arg(desc_params, opts.daemon_login);
  command_line::add_arg(desc_params, opts.testnet);
  command_line::add_arg(desc_params, opts.stagenet);
  command_line::add_arg(desc_params, opts.restricted);
  command_line::add_arg(desc_params, opts.shared_ringdb_dir);
}

std::unique_ptr<wallet> wallet::make_from_json(const boost::program_options::variables_map& vm, const std::string& json_file, const std::function<boost::optional<tools::password_container>(const char *, bool)> &password_prompter)
{
  const options opts{};
  return generate_from_json(json_file, vm, opts, password_prompter);
}

std::pair<std::unique_ptr<wallet>, password_container> wallet::make_from_file(
  const boost::program_options::variables_map& vm, const std::string& wallet_file, const std::function<boost::optional<tools::password_container>(const char *, bool)> &password_prompter)
{
  const options opts{};
  auto pwd = get_password(vm, opts, password_prompter, false);
  if (!pwd)
  {
    return {nullptr, password_container{}};
  }
  auto wallet = make_basic(vm, opts, password_prompter);
  if (wallet)
  {
    wallet->load(wallet_file, pwd->password());
  }
  return {std::move(wallet), std::move(*pwd)};
}

std::pair<std::unique_ptr<wallet>, password_container> wallet::make_new(const boost::program_options::variables_map& vm, const std::function<boost::optional<password_container>(const char *, bool)> &password_prompter)
{
  const options opts{};
  auto pwd = get_password(vm, opts, password_prompter, true);
  if (!pwd)
  {
    return {nullptr, password_container{}};
  }
  return {make_basic(vm, opts, password_prompter), std::move(*pwd)};
}

std::unique_ptr<wallet> wallet::make_dummy(const boost::program_options::variables_map& vm, const std::function<boost::optional<tools::password_container>(const char *, bool)> &password_prompter)
{
  const options opts{};
  return make_basic(vm, opts, password_prompter);
}

//----------------------------------------------------------------------------------------------------
bool wallet::init(std::string daemon_address, boost::optional<epee::net_utils::http::login> daemon_login, uint64_t upper_transaction_size_limit, bool ssl)
{
  m_checkpoints.init_default_checkpoints(m_nettype);
  if(m_http_client.is_connected())
    m_http_client.disconnect();
  m_is_initialized = true;
  m_upper_transaction_size_limit = upper_transaction_size_limit;
  m_daemon_address = std::move(daemon_address);
  m_daemon_login = std::move(daemon_login);
  // When switching from light wallet to full wallet, we need to reset the height we got from lw node.
  if(m_light_wallet)
    m_local_bc_height = m_blockchain.size();
  return m_http_client.set_server(get_daemon_address(), get_daemon_login(), ssl);
}
//----------------------------------------------------------------------------------------------------
bool wallet::is_deterministic() const
{
  crypto::secret_key second;
  keccak((uint8_t *)&get_account().get_keys().m_spend_secret_key, sizeof(crypto::secret_key), (uint8_t *)&second, sizeof(crypto::secret_key));
  sc_reduce32((uint8_t *)&second);
  bool keys_deterministic = memcmp(second.data,get_account().get_keys().m_view_secret_key.data, sizeof(crypto::secret_key)) == 0;
  return keys_deterministic;
}
//----------------------------------------------------------------------------------------------------
bool wallet::get_seed(std::string& electrum_words, const epee::wipeable_string &passphrase) const
{
  bool keys_deterministic = is_deterministic();
  if (!keys_deterministic)
  {
    std::cout << "This is not a deterministic wallet" << std::endl;
    return false;
  }
  if (seed_language.empty())
  {
    std::cout << "seed_language not set" << std::endl;
    return false;
  }

  crypto::secret_key key = get_account().get_keys().m_spend_secret_key;
  if (!passphrase.empty())
    key = cryptonote::encrypt_key(key, passphrase);
  if (!crypto::ElectrumWords::bytes_to_words(key, electrum_words, seed_language))
  {
    std::cout << "Failed to create seed from key for language: " << seed_language << std::endl;
    return false;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
/*!
 * \brief Gets the seed language
 */
const std::string &wallet::get_seed_language() const
{
  return seed_language;
}
/*!
 * \brief Sets the seed language
 * \param language  Seed language to set to
 */
void wallet::set_seed_language(const std::string &language)
{
  seed_language = language;
}
//----------------------------------------------------------------------------------------------------
cryptonote::account_public_address wallet::get_subaddress(const cryptonote::subaddress_index& index) const
{
  hw::device &hwdev = m_account.get_device();
  return hwdev.get_subaddress(m_account.get_keys(), index);
}
//----------------------------------------------------------------------------------------------------
crypto::public_key wallet::get_subaddress_spend_public_key(const cryptonote::subaddress_index& index) const
{
  hw::device &hwdev = m_account.get_device();
  return hwdev.get_subaddress_spend_public_key(m_account.get_keys(), index);
}
//----------------------------------------------------------------------------------------------------
std::string wallet::get_subaddress_as_str(const cryptonote::subaddress_index& index) const
{
  cryptonote::account_public_address address = get_subaddress(index);
  return cryptonote::get_account_address_as_str(m_nettype, !index.is_zero(), address);
}
//----------------------------------------------------------------------------------------------------
std::string wallet::get_integrated_address_as_str(const crypto::hash8& payment_id) const
{
  return cryptonote::get_account_integrated_address_as_str(m_nettype, get_address(), payment_id);
}
//----------------------------------------------------------------------------------------------------
void wallet::add_subaddress_account(const std::string& label)
{
  uint32_t index_major = (uint32_t)get_num_subaddress_accounts();
  expand_subaddresses({index_major, 0});
  m_subaddress_labels[index_major][0] = label;
}
//----------------------------------------------------------------------------------------------------
void wallet::add_subaddress(uint32_t index_major, const std::string& label)
{
  THROW_WALLET_EXCEPTION_IF(index_major >= m_subaddress_labels.size(), error::account_index_outofbound);
  uint32_t index_minor = (uint32_t)get_num_subaddresses(index_major);
  expand_subaddresses({index_major, index_minor});
  m_subaddress_labels[index_major][index_minor] = label;
}
//----------------------------------------------------------------------------------------------------
void wallet::expand_subaddresses(const cryptonote::subaddress_index& index)
{
  hw::device &hwdev = m_account.get_device();
  if (m_subaddress_labels.size() <= index.major)
  {
    // add new accounts
    cryptonote::subaddress_index index2;
    const uint32_t major_end = get_subaddress_clamped_sum(index.major, m_subaddress_lookahead_major);
    for (index2.major = m_subaddress_labels.size(); index2.major < major_end; ++index2.major)
    {
      const uint32_t end = get_subaddress_clamped_sum((index2.major == index.major ? index.minor : 0), m_subaddress_lookahead_minor);
      const std::vector<crypto::public_key> pkeys = hwdev.get_subaddress_spend_public_keys(m_account.get_keys(), index2.major, 0, end);
      for (index2.minor = 0; index2.minor < end; ++index2.minor)
      {
         const crypto::public_key &D = pkeys[index2.minor];
         m_subaddresses[D] = index2;
      }
    }
    m_subaddress_labels.resize(index.major + 1, {"Untitled account"});
    m_subaddress_labels[index.major].resize(index.minor + 1);
  }
  else if (m_subaddress_labels[index.major].size() <= index.minor)
  {
    // add new subaddresses
    const uint32_t end = get_subaddress_clamped_sum(index.minor, m_subaddress_lookahead_minor);
    const uint32_t begin = m_subaddress_labels[index.major].size();
    cryptonote::subaddress_index index2 = {index.major, begin};
    const std::vector<crypto::public_key> pkeys = hwdev.get_subaddress_spend_public_keys(m_account.get_keys(), index2.major, index2.minor, end);
    for (; index2.minor < end; ++index2.minor)
    {
       const crypto::public_key &D = pkeys[index2.minor - begin];
       m_subaddresses[D] = index2;
    }
    m_subaddress_labels[index.major].resize(index.minor + 1);
  }
}
//----------------------------------------------------------------------------------------------------
std::string wallet::get_subaddress_label(const cryptonote::subaddress_index& index) const
{
  if (index.major >= m_subaddress_labels.size() || index.minor >= m_subaddress_labels[index.major].size())
  {
    MERROR("Subaddress label doesn't exist");
    return "";
  }
  return m_subaddress_labels[index.major][index.minor];
}
//----------------------------------------------------------------------------------------------------
void wallet::set_subaddress_label(const cryptonote::subaddress_index& index, const std::string &label)
{
  THROW_WALLET_EXCEPTION_IF(index.major >= m_subaddress_labels.size(), error::account_index_outofbound);
  THROW_WALLET_EXCEPTION_IF(index.minor >= m_subaddress_labels[index.major].size(), error::address_index_outofbound);
  m_subaddress_labels[index.major][index.minor] = label;
}
//----------------------------------------------------------------------------------------------------
void wallet::set_subaddress_lookahead(size_t major, size_t minor)
{
  THROW_WALLET_EXCEPTION_IF(major > 0xffffffff, error::wallet_internal_error, "Subaddress major lookahead is too large");
  THROW_WALLET_EXCEPTION_IF(minor > 0xffffffff, error::wallet_internal_error, "Subaddress minor lookahead is too large");
  m_subaddress_lookahead_major = major;
  m_subaddress_lookahead_minor = minor;
}
//----------------------------------------------------------------------------------------------------
/*!
 * \brief Tells if the wallet file is deprecated.
 */
bool wallet::is_deprecated() const
{
  return is_old_file_format;
}
//----------------------------------------------------------------------------------------------------
void wallet::set_spent(size_t idx, uint64_t height)
{
  transfer_details &td = m_transfers[idx];
  LOG_PRINT_L2("Setting SPENT at " << height << ": ki " << td.m_key_image << ", amount " << print_money(td.m_amount));
  td.m_spent = true;
  td.m_spent_height = height;
}
//----------------------------------------------------------------------------------------------------
void wallet::set_unspent(size_t idx)
{
  transfer_details &td = m_transfers[idx];
  LOG_PRINT_L2("Setting UNSPENT: ki " << td.m_key_image << ", amount " << print_money(td.m_amount));
  td.m_spent = false;
  td.m_spent_height = 0;
}
//----------------------------------------------------------------------------------------------------
void wallet::check_acc_out_precomp(const tx_out &o, const crypto::key_derivation &derivation, const std::vector<crypto::key_derivation> &additional_derivations, size_t i, tx_scan_info_t &tx_scan_info) const
{
  hw::device &hwdev = m_account.get_device();


  boost::unique_lock<hw::device> hwdev_lock (hwdev);
  hwdev.set_mode(hw::device::TRANSACTION_PARSE);
  if (!cryptonote::is_valid_transaction_output_type(o.target))
  {
     tx_scan_info.error = true;
     LOG_ERROR("wrong type id in transaction out");
     return;
  }

  tx_scan_info.token_transfer = cryptonote::is_token_output(o.target);
  const crypto::public_key &out_key = *boost::apply_visitor(destination_public_key_visitor(), o.target);
  if (cryptonote::get_tx_out_type(o.target) == tx_out_type::out_safex_account)
  {
    boost::optional<cryptonote::subaddress_receive_info> result = AUTO_VAL_INIT(result);
    for (auto &sfx_acc_keys: m_safex_accounts_keys)
      if (result = is_safex_output_to_acc_precomp(sfx_acc_keys, m_subaddresses, out_key, i, hwdev))
      {
        tx_scan_info.received = result;
        break;
      }
  }
  else
    tx_scan_info.received = is_out_to_acc_precomp(m_subaddresses, out_key, derivation, additional_derivations, i, hwdev);

  if(tx_scan_info.received)
  {
    tx_scan_info.money_transfered = o.amount; // may be 0 for token outputs
    tx_scan_info.token_transfered = o.token_amount;
    tx_scan_info.output_type = cryptonote::get_tx_out_type(o.target);
  }
  else
  {
    tx_scan_info.money_transfered = 0;
    tx_scan_info.token_transfered = 0;
    tx_scan_info.output_type = cryptonote::tx_out_type::out_invalid;
  }
  tx_scan_info.error = false;
}

//----------------------------------------------------------------------------------------------------
void wallet::scan_output(const cryptonote::transaction &tx, const crypto::public_key &tx_pub_key, size_t i, tx_scan_info_t &tx_scan_info, int &num_vouts_received,
    std::unordered_map<cryptonote::subaddress_index, uint64_t> &tx_money_got_in_outs, std::unordered_map<cryptonote::subaddress_index, uint64_t> &tx_tokens_got_in_outs, std::vector<size_t> &outs) const
{
  THROW_WALLET_EXCEPTION_IF(i >= tx.vout.size(), error::wallet_internal_error, "Invalid vout index");

  //get key
  const crypto::public_key &out_key = *boost::apply_visitor(destination_public_key_visitor(), tx.vout[i].target);
  bool r = cryptonote::generate_key_image_helper_precomp(m_account.get_keys(), out_key, tx_scan_info.received->derivation, i, tx_scan_info.received->index, tx_scan_info.in_ephemeral, tx_scan_info.ki, m_account.get_device());
  THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key image");
  THROW_WALLET_EXCEPTION_IF(tx_scan_info.in_ephemeral.pub != out_key,
      error::wallet_internal_error, "key_image generated ephemeral public key not matched with output_key");


  outs.push_back(i);
  if (tx_scan_info.token_transfer && tx_scan_info.output_type == tx_out_type::out_token)
  {
    tx_tokens_got_in_outs[tx_scan_info.received->index] += tx_scan_info.token_transfered;
    tx_scan_info.token_amount = tx_scan_info.token_transfered;
  }
  else
  {
    tx_money_got_in_outs[tx_scan_info.received->index] += tx_scan_info.money_transfered;
    tx_scan_info.amount = tx_scan_info.money_transfered;
  }

  ++num_vouts_received;
}
//----------------------------------------------------------------------------------------------------
void wallet::process_new_transaction(const crypto::hash &txid, const cryptonote::transaction& tx, const std::vector<uint64_t> &o_indices, uint64_t height, uint64_t ts, bool miner_tx, bool pool, bool double_spend_seen)
{
  //ensure device is let in NONE mode in any case
  hw::device &hwdev = m_account.get_device();

  boost::unique_lock<hw::device> hwdev_lock (hwdev);
  hw::reset_mode rst(hwdev);
  hwdev_lock.unlock();

 // In this function, tx (probably) only contains the base information
  // (that is, the prunable stuff may or may not be included)
  if (!miner_tx && !pool)
    process_unconfirmed(txid, tx, height);
  std::vector<size_t> outs;
  std::unordered_map<cryptonote::subaddress_index, uint64_t> tx_money_got_in_outs;  // per receiving subaddress index
  std::unordered_map<cryptonote::subaddress_index, uint64_t> tx_tokens_got_in_outs;  // per receiving subaddress index
  crypto::public_key tx_pub_key = null_pkey;

  std::vector<tx_extra_field> tx_extra_fields;
  if(!parse_tx_extra(tx.extra, tx_extra_fields))
  {
    // Extra may only be partially parsed, it's OK if tx_extra_fields contains public key
    LOG_PRINT_L0("Transaction extra has unsupported format: " << txid);
  }

  // Don't try to extract tx public key if tx has no ouputs
  size_t pk_index = 0;
  std::vector<tx_scan_info_t> tx_scan_info(tx.vout.size());
  uint64_t total_received_1 = 0;
  uint64_t total_token_received_1 = 0;
  while (!tx.vout.empty())
  {
    // if tx.vout is not empty, we loop through all tx pubkeys

    tx_extra_pub_key pub_key_field;
    if(!find_tx_extra_field_by_type(tx_extra_fields, pub_key_field, pk_index++))
    {
      if (pk_index > 1)
        break;
      LOG_PRINT_L0("Public key wasn't found in the transaction extra. Skipping transaction " << txid);
      if(0 != m_callback)
	m_callback->on_skip_transaction(height, txid, tx);
      break;
    }

    int num_vouts_received = 0;
    tx_pub_key = pub_key_field.pub_key;
    tools::threadpool& tpool = tools::threadpool::getInstance();
    tools::threadpool::waiter waiter;
    const cryptonote::account_keys& keys = m_account.get_keys();
    crypto::key_derivation derivation;

    hwdev_lock.lock();
    hwdev.set_mode(hw::device::TRANSACTION_PARSE);
    if (!hwdev.generate_key_derivation(tx_pub_key, keys.m_view_secret_key, derivation))
    {
      MWARNING("Failed to generate key derivation from tx pubkey, skipping");
      static_assert(sizeof(derivation) == sizeof(rct::key), "Mismatched sizes of key_derivation and rct::key");
      memcpy(&derivation, rct::identity().bytes, sizeof(derivation));
    }

    // additional tx pubkeys and derivations for multi-destination transfers involving one or more subaddresses
    std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(tx);
    std::vector<crypto::key_derivation> additional_derivations;
    for (size_t i = 0; i < additional_tx_pub_keys.size(); ++i)
    {
      additional_derivations.push_back({});
      if (!hwdev.generate_key_derivation(additional_tx_pub_keys[i], keys.m_view_secret_key, additional_derivations.back()))
      {
        MWARNING("Failed to generate key derivation from tx pubkey, skipping");
        additional_derivations.pop_back();
      }
    }
    hwdev_lock.unlock();

    if (miner_tx && m_refresh_type == RefreshNoCoinbase)
    {
      // assume coinbase isn't for us
    }
    else if (miner_tx && m_refresh_type == RefreshOptimizeCoinbase)
    {
      check_acc_out_precomp(tx.vout[0], derivation, additional_derivations, 0, tx_scan_info[0]);
      THROW_WALLET_EXCEPTION_IF(tx_scan_info[0].error, error::acc_outs_lookup_error, tx, tx_pub_key, m_account.get_keys());

      // this assumes that the miner tx pays a single address
      if (tx_scan_info[0].received)
      {
        // process the other outs from that tx
        // the first one was already checked
        for (size_t i = 1; i < tx.vout.size(); ++i)
        {
          tpool.submit(&waiter, boost::bind(&wallet::check_acc_out_precomp, this, std::cref(tx.vout[i]), std::cref(derivation), std::cref(additional_derivations), i,
            std::ref(tx_scan_info[i])));
        }
        waiter.wait();
        // then scan all outputs from 0
        hwdev_lock.lock();
        hwdev.set_mode(hw::device::NONE);
        for (size_t i = 0; i < tx.vout.size(); ++i)
        {
          THROW_WALLET_EXCEPTION_IF(tx_scan_info[i].error, error::acc_outs_lookup_error, tx, tx_pub_key, m_account.get_keys());
          if (tx_scan_info[i].received)
          {
            if (tx_scan_info[i].output_type == tx_out_type::out_safex_account) {
              outs.push_back(i);
              continue;
            }

            hwdev.conceal_derivation(tx_scan_info[i].received->derivation, tx_pub_key, additional_tx_pub_keys, derivation, additional_derivations);
            scan_output(tx, tx_pub_key, i, tx_scan_info[i], num_vouts_received, tx_money_got_in_outs, tx_tokens_got_in_outs, outs);
          }
        }
        hwdev_lock.unlock();
      }
    }
    else if (tx.vout.size() > 1 && tools::threadpool::getInstance().get_max_concurrency() > 1)
    {
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        tpool.submit(&waiter, boost::bind(&wallet::check_acc_out_precomp, this, std::cref(tx.vout[i]), std::cref(derivation), std::cref(additional_derivations), i,
            std::ref(tx_scan_info[i])));
      }
      waiter.wait();

      hwdev_lock.lock();
      hwdev.set_mode(hw::device::NONE);
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        THROW_WALLET_EXCEPTION_IF(tx_scan_info[i].error, error::acc_outs_lookup_error, tx, tx_pub_key, m_account.get_keys());
        if (tx_scan_info[i].received)
        {
          if (tx_scan_info[i].output_type == tx_out_type::out_safex_account) {
            outs.push_back(i);
            continue;
          }
          hwdev.conceal_derivation(tx_scan_info[i].received->derivation, tx_pub_key, additional_tx_pub_keys, derivation, additional_derivations);
          scan_output(tx, tx_pub_key, i, tx_scan_info[i], num_vouts_received, tx_money_got_in_outs, tx_tokens_got_in_outs, outs);
        }
      }
      hwdev_lock.unlock();
    }
    else
    {
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        check_acc_out_precomp(tx.vout[i], derivation, additional_derivations, i, tx_scan_info[i]);
        THROW_WALLET_EXCEPTION_IF(tx_scan_info[i].error, error::acc_outs_lookup_error, tx, tx_pub_key, m_account.get_keys());
        if (tx_scan_info[i].received)
        {
          hwdev_lock.lock();
          hwdev.set_mode(hw::device::NONE);
          hwdev.conceal_derivation(tx_scan_info[i].received->derivation, tx_pub_key, additional_tx_pub_keys, derivation, additional_derivations);
          scan_output(tx, tx_pub_key, i, tx_scan_info[i], num_vouts_received, tx_money_got_in_outs, tx_tokens_got_in_outs, outs);
          hwdev_lock.unlock();
        }
      }
    }

    if(!outs.empty() && num_vouts_received > 0)
    {

      //good news - got money! take care about it
      //usually we have only one transfer for user in transaction
      if (!pool)
      {
        THROW_WALLET_EXCEPTION_IF(tx.vout.size() != o_indices.size(), error::wallet_internal_error,
            "transactions outputs size=" + std::to_string(tx.vout.size()) +
            " not match with daemon response size=" + std::to_string(o_indices.size()));
      }

      for(size_t o: outs)
      {

        THROW_WALLET_EXCEPTION_IF(tx.vout.size() <= o, error::wallet_internal_error, "wrong out in transaction: internal index=" +
            std::to_string(o) + ", total_outs=" + std::to_string(tx.vout.size()));

        auto kit = m_pub_keys.find(tx_scan_info[o].in_ephemeral.pub);
        THROW_WALLET_EXCEPTION_IF(kit != m_pub_keys.end() && kit->second >= m_transfers.size(),
            error::wallet_internal_error, std::string("Unexpected transfer index from public key: ")
        + "got " + (kit == m_pub_keys.end() ? "<none>" : boost::lexical_cast<std::string>(kit->second))
        + ", m_transfers.size() is " + boost::lexical_cast<std::string>(m_transfers.size()));
        if (kit == m_pub_keys.end())
        {
          uint64_t amount = tx.vout[o].amount ? tx.vout[o].amount : tx_scan_info[o].amount;
          uint64_t token_amount = tx.vout[o].token_amount ? tx.vout[o].token_amount : tx_scan_info[o].token_amount;
          tx_out_type output_type = cryptonote::get_tx_out_type(tx.vout[o].target);
          if (!pool)
          {
            m_transfers.push_back(boost::value_initialized<transfer_details>());
            transfer_details& td = m_transfers.back();
            td.m_block_height = height;
            td.m_internal_output_index = o;
            td.m_global_output_index = o_indices[o];
            td.m_tx = (const cryptonote::transaction_prefix&)tx;
            td.m_txid = txid;
            td.m_key_image = tx_scan_info[o].ki;
            td.m_key_image_known = !m_watch_only;
            td.m_key_image_partial = m_multisig;
            td.m_amount = amount;
            td.m_token_amount = token_amount;
            td.m_token_transfer = td.m_token_amount > 0;
            td.m_output_type = output_type;
            td.m_pk_index = pk_index - 1;
            td.m_subaddr_index = tx_scan_info[o].received->index;
            expand_subaddresses(tx_scan_info[o].received->index);

            td.m_mask = rct::identity();
            td.m_rct = false;

            set_unspent(m_transfers.size()-1);
            if (!m_watch_only)
              m_key_images[td.m_key_image] = m_transfers.size()-1;
            m_pub_keys[tx_scan_info[o].in_ephemeral.pub] = m_transfers.size()-1;
            if (td.m_token_transfer)
              LOG_PRINT_L0("Received tokens: " << print_money(td.token_amount()) << ", with tx: " << txid);
            else
              LOG_PRINT_L0("Received money: " << print_money(td.amount()) << ", with tx: " << txid);

            if (0 != m_callback) {
              if (td.m_token_transfer)
                m_callback->on_tokens_received(height, txid, tx, td.m_token_amount, td.m_subaddr_index);
              else if (output_type == tx_out_type::out_safex_account) {
                const txout_to_script &txout = boost::get<txout_to_script>(tx.vout[o].target);
                m_callback->on_advanced_output_received(height, txid, tx, txout, td.m_subaddr_index);
              }
              else
                m_callback->on_money_received(height, txid, tx, td.m_amount, td.m_subaddr_index);
            }
          }
          total_received_1 += amount;
          total_token_received_1 += token_amount;
        }
        else if (m_transfers[kit->second].m_spent || m_transfers[kit->second].amount() >= tx.vout[o].amount || m_transfers[kit->second].token_amount() >= tx.vout[o].token_amount)
        {
          if (m_transfers[kit->second].m_token_transfer) {
            LOG_ERROR("Public key " << epee::string_tools::pod_to_hex(kit->first)
            << " from received " << print_money(tx.vout[o].token_amount) << " token output already exists with "
            << (m_transfers[kit->second].m_spent ? "spent" : "unspent") << " "
            << print_money(m_transfers[kit->second].token_amount()) << ", received token output ignored");
            THROW_WALLET_EXCEPTION_IF(tx_tokens_got_in_outs[tx_scan_info[o].received->index] < tx_scan_info[o].token_amount,
                                      error::wallet_internal_error, "Unexpected values of new and old token outputs");
            tx_tokens_got_in_outs[tx_scan_info[o].received->index] -= tx_scan_info[o].token_amount;
          } else {
            LOG_ERROR("Public key " << epee::string_tools::pod_to_hex(kit->first)
            << " from received " << print_money(tx.vout[o].amount) << " output already exists with "
            << (m_transfers[kit->second].m_spent ? "spent" : "unspent") << " "
            << print_money(m_transfers[kit->second].amount()) << ", received output ignored");
            THROW_WALLET_EXCEPTION_IF(tx_money_got_in_outs[tx_scan_info[o].received->index] < tx_scan_info[o].amount,
                                      error::wallet_internal_error, "Unexpected values of new and old outputs");
            tx_money_got_in_outs[tx_scan_info[o].received->index] -= tx_scan_info[o].amount;

          }
        }
        else
        {
          uint64_t token_amount = 0;
          uint64_t extra_token_amount = 0;
          uint64_t amount = 0;
          uint64_t extra_amount = 0;

          if (m_transfers[kit->second].m_token_transfer) {
            LOG_ERROR("Public key " << epee::string_tools::pod_to_hex(kit->first)
            << " from received " << print_money(tx.vout[o].token_amount) << " token output already exists with "
            << print_money(m_transfers[kit->second].token_amount()) << ", replacing with new token output");

            // The new larger output replaced a previous smaller one
            THROW_WALLET_EXCEPTION_IF(tx_tokens_got_in_outs[tx_scan_info[o].received->index] < tx_scan_info[o].token_amount,
                                      error::wallet_internal_error, "Unexpected values of new and old token outputs");
            THROW_WALLET_EXCEPTION_IF(m_transfers[kit->second].token_amount() > tx_scan_info[o].token_amount,
                                      error::wallet_internal_error, "Unexpected values of new and old token outputs");
            tx_tokens_got_in_outs[tx_scan_info[o].received->index] -= m_transfers[kit->second].token_amount();
            token_amount = tx.vout[o].token_amount ? tx.vout[o].token_amount : tx_scan_info[o].token_amount;
            extra_token_amount = token_amount - m_transfers[kit->second].token_amount();

          } else {
            LOG_ERROR("Public key " << epee::string_tools::pod_to_hex(kit->first)
            << " from received " << print_money(tx.vout[o].amount) << " output already exists with "
            << print_money(m_transfers[kit->second].amount()) << ", replacing with new output");

            // The new larger output replaced a previous smaller one
            THROW_WALLET_EXCEPTION_IF(tx_money_got_in_outs[tx_scan_info[o].received->index] < tx_scan_info[o].amount,
                                      error::wallet_internal_error, "Unexpected values of new and old outputs");
            THROW_WALLET_EXCEPTION_IF(m_transfers[kit->second].amount() > tx_scan_info[o].amount,
                                      error::wallet_internal_error, "Unexpected values of new and old outputs");
            tx_money_got_in_outs[tx_scan_info[o].received->index] -= m_transfers[kit->second].amount();
            amount = tx.vout[o].amount ? tx.vout[o].amount : tx_scan_info[o].amount;
            extra_amount = amount - m_transfers[kit->second].amount();
          }

          if (!pool)
          {
            transfer_details &td = m_transfers[kit->second];
            td.m_block_height = height;
            td.m_internal_output_index = o;
            td.m_global_output_index = o_indices[o];
            td.m_tx = (const cryptonote::transaction_prefix&)tx;
            td.m_txid = txid;
            td.m_amount = amount;
            td.m_token_amount = token_amount;
            td.m_token_transfer = td.m_token_amount > 0;
            td.m_pk_index = pk_index - 1;
            td.m_subaddr_index = tx_scan_info[o].received->index;
            expand_subaddresses(tx_scan_info[o].received->index);
            if ((tx.vout[o].amount == 0) && (tx.vout[o].token_amount == 0))
            {
              td.m_mask = tx_scan_info[o].mask;
              td.m_amount = tx_scan_info[o].amount;
              td.m_token_amount = tx_scan_info[o].token_amount;
              td.m_rct = true;
            }
            else if (miner_tx && tx.version == 2)
            {
              td.m_mask = rct::identity();
              td.m_rct = true;
            }
            else
            {
              td.m_mask = rct::identity();
              td.m_rct = false;
            }
            THROW_WALLET_EXCEPTION_IF(td.get_public_key() != tx_scan_info[o].in_ephemeral.pub, error::wallet_internal_error, "Inconsistent public keys");
            THROW_WALLET_EXCEPTION_IF(td.m_spent, error::wallet_internal_error, "Inconsistent spent status");

            if (td.m_token_transfer)
              LOG_PRINT_L0("Received tokens: " << print_money(td.token_amount()) << ", with tx: " << txid);
            else
              LOG_PRINT_L0("Received money: " << print_money(td.amount()) << ", with tx: " << txid);

            if (0 != m_callback) {
              if (td.m_token_transfer)
                m_callback->on_tokens_received(height, txid, tx, td.m_token_amount, td.m_subaddr_index);
              else
                m_callback->on_money_received(height, txid, tx, td.m_amount, td.m_subaddr_index);
            }

            total_received_1 += extra_amount;
            total_token_received_1 += extra_token_amount;
          }
        }
      }
    }
  }

  uint64_t tx_money_spent_in_ins = 0;
  uint64_t tx_tokens_spent_in_ins = 0;
  // The line below is equivalent to "boost::optional<uint32_t> subaddr_account;", but avoids the GCC warning: ‘*((void*)& subaddr_account +4)’ may be used uninitialized in this function
  // It's a GCC bug with boost::optional, see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=47679
  auto subaddr_account ([]()->boost::optional<uint32_t> {return boost::none;}());
  std::set<uint32_t> subaddr_indices;
  // check all outputs for spending (compare key images)
  for(auto& in: tx.vin)
  {
    if(!cryptonote::is_valid_transaction_input_type(in, tx.version))
      continue;

    const crypto::key_image &k_image = *boost::apply_visitor(key_image_visitor(), in);

    auto it = m_key_images.find(k_image);
    if(it != m_key_images.end())
    {
      transfer_details& td = m_transfers[it->second];
      uint64_t value_amount = *boost::apply_visitor(amount_visitor(), in);

      if (td.m_token_transfer) {
        if (value_amount > 0)
        {

          if(value_amount != td.token_amount())
          {
            MERROR("Inconsistent token amount in tx input: got " << print_money(value_amount) << ", expected " << print_money(td.token_amount()));
            td.m_token_amount = value_amount;
          }
        }
        else
        {
          value_amount = td.token_amount();
        }
        tx_tokens_spent_in_ins += value_amount;
      } else {
        if (value_amount > 0)
        {
          if(value_amount != td.amount())
          {
            MERROR("Inconsistent amount in tx input: got " << print_money(value_amount) << ", expected " << print_money(td.amount()));
            // this means:
            //   1) the same output pub key was used as destination multiple times,
            //   2) the wallet set the highest amount among them to transfer_details::m_amount, and
            //   3) the wallet somehow spent that output with an amount smaller than the above amount, causing inconsistency
            td.m_amount = value_amount;
          }
        }
        else
        {
          value_amount = td.amount();
        }
        tx_money_spent_in_ins += value_amount;
      }

      if (subaddr_account && *subaddr_account != td.m_subaddr_index.major)
        LOG_ERROR("spent funds are from different subaddress accounts; count of incoming/outgoing payments will be incorrect");
      subaddr_account = td.m_subaddr_index.major;
      subaddr_indices.insert(td.m_subaddr_index.minor);
      if (!pool)
      {
        LOG_PRINT_L0("Spent " << (td.m_token_transfer ? "tokens:" : "money:") << print_money(value_amount) << ", with tx: " << txid);
        set_spent(it->second, height);
        if (0 != m_callback) {
          if (td.m_token_transfer)
            m_callback->on_tokens_spent(height, txid, tx, value_amount, tx, td.m_subaddr_index);
          else
            m_callback->on_money_spent(height, txid, tx, value_amount, tx, td.m_subaddr_index);
        }
      }
    }
  }

  uint64_t fee = miner_tx ? 0 : tx.version == 1 ? tx_money_spent_in_ins - get_outs_cash_amount(tx) : tx.rct_signatures.txnFee;

  if ((tx_money_spent_in_ins > 0 || tx_tokens_spent_in_ins > 0) && !pool)
  {
    uint64_t self_received = std::accumulate<decltype(tx_money_got_in_outs.begin()), uint64_t>(tx_money_got_in_outs.begin(), tx_money_got_in_outs.end(), 0,
        [&subaddr_account] (uint64_t acc, const std::pair<cryptonote::subaddress_index, uint64_t>& p)
        {
          return acc + (p.first.major == *subaddr_account ? p.second : 0);
        });
    uint64_t self_token_received = std::accumulate<decltype(tx_tokens_got_in_outs.begin()), uint64_t>(tx_tokens_got_in_outs.begin(), tx_tokens_got_in_outs.end(), 0,
        [&subaddr_account] (uint64_t acc, const std::pair<cryptonote::subaddress_index, uint64_t>& p)
        {
          return acc + (p.first.major == *subaddr_account ? p.second : 0);
        });
    process_outgoing(txid, tx, height, ts, tx_money_spent_in_ins, tx_tokens_spent_in_ins, self_received, self_token_received, *subaddr_account, subaddr_indices);
    // if sending to yourself at the same subaddress account, set the outgoing payment amount to 0 so that it's less confusing
    if (tx_money_spent_in_ins == self_received + fee)
    {
      auto i = m_confirmed_txs.find(txid);
      THROW_WALLET_EXCEPTION_IF(i == m_confirmed_txs.end(), error::wallet_internal_error,
          "confirmed tx wasn't found: " + string_tools::pod_to_hex(txid));
      i->second.m_change = self_received;
    }

    if (tx_tokens_spent_in_ins == self_token_received)
    {
      auto i = m_confirmed_txs.find(txid);
      THROW_WALLET_EXCEPTION_IF(i == m_confirmed_txs.end(), error::wallet_internal_error,
          "confirmed tx wasn't found: " + string_tools::pod_to_hex(txid));
      i->second.m_token_change = self_token_received;
    }
  }

  // remove change sent to the spending subaddress account from the list of received funds
  for (auto i = tx_money_got_in_outs.begin(); i != tx_money_got_in_outs.end();)
  {
    if (subaddr_account && i->first.major == *subaddr_account)
      i = tx_money_got_in_outs.erase(i);
    else
      ++i;
  }

  // remove change sent to the spending subaddress account from the list of received funds
  for (auto i = tx_tokens_got_in_outs.begin(); i != tx_tokens_got_in_outs.end();)
  {
    if (subaddr_account && i->first.major == *subaddr_account)
      i = tx_tokens_got_in_outs.erase(i);
    else
      ++i;
  }

  // create payment_details for each incoming transfer to a subaddress index
  if ((tx_money_got_in_outs.size() > 0) || (tx_tokens_got_in_outs.size() > 0))
  {
    tx_extra_nonce extra_nonce;
    crypto::hash payment_id = null_hash;
    if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
    {
      crypto::hash8 payment_id8 = null_hash8;
      if(get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
      {
        // We got a payment ID to go with this tx
        LOG_PRINT_L2("Found encrypted payment ID: " << payment_id8);
        if (tx_pub_key != null_pkey)
        {
          if (!m_account.get_device().decrypt_payment_id(payment_id8, tx_pub_key, m_account.get_keys().m_view_secret_key))
          {
            LOG_PRINT_L0("Failed to decrypt payment ID: " << payment_id8);
          }
          else
          {
            LOG_PRINT_L2("Decrypted payment ID: " << payment_id8);
            // put the 64 bit decrypted payment id in the first 8 bytes
            memcpy(payment_id.data, payment_id8.data, 8);
            // rest is already 0, but guard against code changes above
            memset(payment_id.data + 8, 0, 24);
          }
        }
        else
        {
          LOG_PRINT_L1("No public key found in tx, unable to decrypt payment id");
        }
      }
      else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
      {
        LOG_PRINT_L2("Found unencrypted payment ID: " << payment_id);
      }
    }
    else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
    {
      LOG_PRINT_L2("Found unencrypted payment ID: " << payment_id);
    }

    uint64_t total_received_2 = 0;
    uint64_t total_token_received_2 = 0;
    for (const auto& i : tx_money_got_in_outs)
      total_received_2 += i.second;
    for (const auto& i : tx_tokens_got_in_outs)
      total_token_received_2 += i.second;
    if ((total_received_1 != total_received_2) || (total_token_received_1 != total_token_received_2))
    {
      const el::Level level = el::Level::Warning;
      MCLOG_RED(level, "global", "**********************************************************************");
      MCLOG_RED(level, "global", "Consistency failure in amounts/token amounts received");
      MCLOG_RED(level, "global", "Check transaction " << txid);
      MCLOG_RED(level, "global", "**********************************************************************");
      exit(1);
      return;
    }


    for (const auto& i : tx_money_got_in_outs)
    {
      payment_details payment = AUTO_VAL_INIT(payment);
      payment.m_tx_hash      = txid;
      payment.m_fee          = fee;
      payment.m_amount       = i.second;
      payment.m_block_height = height;
      payment.m_unlock_time  = tx.unlock_time;
      payment.m_timestamp    = ts;
      payment.m_subaddr_index = i.first;
      if (pool) {
        emplace_or_replace(m_unconfirmed_payments, payment_id, pool_payment_details{payment, double_spend_seen});
        if (0 != m_callback)
          m_callback->on_unconfirmed_money_received(height, txid, tx, payment.m_amount, payment.m_subaddr_index);
      }
      else
        m_payments.emplace(payment_id, payment);
      LOG_PRINT_L2("Payment found in " << (pool ? "pool" : "block") << ": " << payment_id << " / " << payment.m_tx_hash << " / " << payment.m_amount);
    }

    for (const auto& i : tx_tokens_got_in_outs)
    {
      payment_details payment = AUTO_VAL_INIT(payment);
      payment.m_tx_hash      = txid;
      payment.m_fee          = fee;
      payment.m_token_amount = i.second;
      payment.m_block_height = height;
      payment.m_unlock_time  = tx.unlock_time;
      payment.m_timestamp    = ts;
      payment.m_subaddr_index = i.first;
      if (pool) {
        emplace_or_replace(m_unconfirmed_payments, payment_id, pool_payment_details{payment, double_spend_seen});
        if (0 != m_callback)
          m_callback->on_unconfirmed_tokens_received(height, txid, tx, payment.m_token_amount, payment.m_subaddr_index);
      }
      else
        m_payments.emplace(payment_id, payment);
      LOG_PRINT_L2("Token payment found in " << (pool ? "pool" : "block") << ": " << payment_id << " / " << payment.m_tx_hash << " / " << payment.m_token_amount);
    }
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::process_unconfirmed(const crypto::hash &txid, const cryptonote::transaction& tx, uint64_t height)
{
  if (m_unconfirmed_txs.empty())
    return;

  auto unconf_it = m_unconfirmed_txs.find(txid);
  if(unconf_it != m_unconfirmed_txs.end()) {
    if (store_tx_info()) {
      try {
        m_confirmed_txs.insert(std::make_pair(txid, confirmed_transfer_details(unconf_it->second, height)));
      }
      catch (...) {
        // can fail if the tx has unexpected input types
        LOG_PRINT_L0("Failed to add outgoing transaction to confirmed transaction map");
      }
    }
    m_unconfirmed_txs.erase(unconf_it);
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::process_outgoing(const crypto::hash &txid, const cryptonote::transaction &tx, uint64_t height, uint64_t ts, uint64_t spent, uint64_t tokens_spent, uint64_t received, uint64_t tokens_received,
    uint32_t subaddr_account, const std::set<uint32_t>& subaddr_indices)
{
  std::pair<std::unordered_map<crypto::hash, confirmed_transfer_details>::iterator, bool> entry = m_confirmed_txs.insert(std::make_pair(txid, confirmed_transfer_details()));
  // fill with the info we know, some info might already be there
  if (entry.second)
  {
    // this case will happen if the tx is from our outputs, but was sent by another
    // wallet (eg, we're a cold wallet and the hot wallet sent it). For RCT transactions,
    // we only see 0 input amounts, so have to deduce amount out from other parameters.
    entry.first->second.m_amount_in = spent;
    entry.first->second.m_token_amount_in = tokens_spent;
    if (tx.version == 1)
    {
      entry.first->second.m_amount_out = get_outs_cash_amount(tx);
      entry.first->second.m_token_amount_out = get_outs_token_amount(tx);
    }
    else
    {
      entry.first->second.m_amount_out = spent - tx.rct_signatures.txnFee;
    }
    entry.first->second.m_change = received;
    entry.first->second.m_token_change = tokens_received;

    std::vector<tx_extra_field> tx_extra_fields;
    parse_tx_extra(tx.extra, tx_extra_fields); // ok if partially parsed
    tx_extra_nonce extra_nonce;
    if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
    {
      // we do not care about failure here
      get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, entry.first->second.m_payment_id);
    }
    entry.first->second.m_subaddr_account = subaddr_account;
    entry.first->second.m_subaddr_indices = subaddr_indices;
  }

  for (const auto &in: tx.vin)
  {
    if ((in.type() != typeid(txin_to_key)) && (in.type() != typeid(txin_token_to_key)))
      continue;

    const crypto::key_image &k_image = *boost::apply_visitor(key_image_visitor(), in);
    const std::vector<uint64_t> &key_offsets = *boost::apply_visitor(key_offset_visitor(), in);

    entry.first->second.m_rings.push_back(std::make_pair(k_image, key_offsets));
  }
  entry.first->second.m_block_height = height;
  entry.first->second.m_timestamp = ts;
  entry.first->second.m_unlock_time = tx.unlock_time;

  add_rings(tx);
}
//----------------------------------------------------------------------------------------------------
void wallet::process_new_blockchain_entry(const cryptonote::block& b, const cryptonote::block_complete_entry& bche, const crypto::hash& bl_id, uint64_t height, const cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::block_output_indices &o_indices)
{
  size_t txidx = 0;
  THROW_WALLET_EXCEPTION_IF(bche.txs.size() + 1 != o_indices.indices.size(), error::wallet_internal_error,
      "block transactions=" + std::to_string(bche.txs.size()) +
      " not match with daemon response size=" + std::to_string(o_indices.indices.size()));

  //handle transactions from new block

  //optimization: seeking only for blocks that are not older then the wallet creation time plus 1 day. 1 day is for possible user incorrect time setup
  if(b.timestamp + 60*60*24 > m_account.get_createtime() && height >= m_refresh_from_block_height)
  {
    TIME_MEASURE_START(miner_tx_handle_time);
    process_new_transaction(get_transaction_hash(b.miner_tx), b.miner_tx, o_indices.indices[txidx++].indices, height, b.timestamp, true, false, false);
    TIME_MEASURE_FINISH(miner_tx_handle_time);

    TIME_MEASURE_START(txs_handle_time);
    THROW_WALLET_EXCEPTION_IF(bche.txs.size() != b.tx_hashes.size(), error::wallet_internal_error, "Wrong amount of transactions for block");
    size_t idx = 0;
    for (const auto& txblob: bche.txs)
    {
      cryptonote::transaction tx;
      bool r = parse_and_validate_tx_base_from_blob(txblob, tx);
      THROW_WALLET_EXCEPTION_IF(!r, error::tx_parse_error, txblob);
      process_new_transaction(b.tx_hashes[idx], tx, o_indices.indices[txidx++].indices, height, b.timestamp, false, false, false);
      ++idx;
    }
    TIME_MEASURE_FINISH(txs_handle_time);
    LOG_PRINT_L2("Processed block: " << bl_id << ", height " << height << ", " <<  miner_tx_handle_time + txs_handle_time << "(" << miner_tx_handle_time << "/" << txs_handle_time <<")ms");
  }else
  {
    if (!(height % 100))
      LOG_PRINT_L2( "Skipped block by timestamp, height: " << height << ", block time " << b.timestamp << ", account time " << m_account.get_createtime());
  }
  m_blockchain.push_back(bl_id);
  ++m_local_bc_height;

  if (0 != m_callback)
    m_callback->on_new_block(height, b);
}
//----------------------------------------------------------------------------------------------------
void wallet::get_short_chain_history(std::list<crypto::hash>& ids) const
{
  size_t i = 0;
  size_t current_multiplier = 1;
  size_t sz = m_blockchain.size() - m_blockchain.offset();
  if(!sz)
  {
    ids.push_back(m_blockchain.genesis());
    return;
  }
  size_t current_back_offset = 1;
  bool base_included = false;
  while(current_back_offset < sz)
  {
    ids.push_back(m_blockchain[m_blockchain.offset() + sz-current_back_offset]);
    if(sz-current_back_offset == 0)
      base_included = true;
    if(i < 10)
    {
      ++current_back_offset;
    }else
    {
      current_back_offset += current_multiplier *= 2;
    }
    ++i;
  }
  if(!base_included)
    ids.push_back(m_blockchain[m_blockchain.offset()]);
  if(m_blockchain.offset())
    ids.push_back(m_blockchain.genesis());
}
//----------------------------------------------------------------------------------------------------
void wallet::parse_block_round(const cryptonote::blobdata &blob, cryptonote::block &bl, crypto::hash &bl_id, bool &error) const
{
  error = !cryptonote::parse_and_validate_block_from_blob(blob, bl);
  if (!error)
    bl_id = get_block_hash(bl);
}
//----------------------------------------------------------------------------------------------------
void wallet::pull_blocks(uint64_t start_height, uint64_t &blocks_start_height, const std::list<crypto::hash> &short_chain_history, std::list<cryptonote::block_complete_entry> &blocks, std::vector<cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::block_output_indices> &o_indices)
{
  cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::request req = AUTO_VAL_INIT(req);
  cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::response res = AUTO_VAL_INIT(res);
  req.block_ids = short_chain_history;

  uint32_t rpc_version;
  boost::optional<std::string> result = m_node_rpc_proxy.get_rpc_version(rpc_version);
  // no error
  if (!!result)
  {
    // empty string -> not connection
    THROW_WALLET_EXCEPTION_IF(result->empty(), tools::error::no_connection_to_daemon, "getversion");
    THROW_WALLET_EXCEPTION_IF(*result == CORE_RPC_STATUS_BUSY, tools::error::daemon_busy, "getversion");
    if (*result != CORE_RPC_STATUS_OK)
    {
      MDEBUG("Cannot determine daemon RPC version, not asking for pruned blocks");
      req.prune = false; // old daemon
    }
  }
  else
  {
    if (rpc_version >= MAKE_CORE_RPC_VERSION(1, 7))
    {
      MDEBUG("Daemon is recent enough, asking for pruned blocks");
      req.prune = true;
    }
    else
    {
      MDEBUG("Daemon is too old, not asking for pruned blocks");
      req.prune = false;
    }
  }

  req.start_height = start_height;
  m_daemon_rpc_mutex.lock();
  bool r = net_utils::invoke_http_bin("/getblocks.bin", req, res, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "getblocks.bin");
  THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "getblocks.bin");
  THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::get_blocks_error, res.status);
  THROW_WALLET_EXCEPTION_IF(res.blocks.size() != res.output_indices.size(), error::wallet_internal_error,
      "mismatched blocks (" + boost::lexical_cast<std::string>(res.blocks.size()) + ") and output_indices (" +
      boost::lexical_cast<std::string>(res.output_indices.size()) + ") sizes from daemon");

  blocks_start_height = res.start_height;
  blocks = res.blocks;
  o_indices = res.output_indices;
}
//----------------------------------------------------------------------------------------------------
void wallet::pull_hashes(uint64_t start_height, uint64_t &blocks_start_height, const std::list<crypto::hash> &short_chain_history, std::list<crypto::hash> &hashes)
{
  cryptonote::COMMAND_RPC_GET_HASHES_FAST::request req = AUTO_VAL_INIT(req);
  cryptonote::COMMAND_RPC_GET_HASHES_FAST::response res = AUTO_VAL_INIT(res);
  req.block_ids = short_chain_history;

  req.start_height = start_height;
  m_daemon_rpc_mutex.lock();
  bool r = net_utils::invoke_http_bin("/gethashes.bin", req, res, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "gethashes.bin");
  THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "gethashes.bin");
  THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::get_hashes_error, res.status);

  blocks_start_height = res.start_height;
  hashes = res.m_block_ids;
}
//----------------------------------------------------------------------------------------------------
void wallet::process_blocks(uint64_t start_height, const std::list<cryptonote::block_complete_entry> &blocks, const std::vector<cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::block_output_indices> &o_indices, uint64_t& blocks_added)
{
  size_t current_index = start_height;
  blocks_added = 0;
  size_t tx_o_indices_idx = 0;

  THROW_WALLET_EXCEPTION_IF(blocks.size() != o_indices.size(), error::wallet_internal_error, "size mismatch");
  THROW_WALLET_EXCEPTION_IF(!m_blockchain.is_in_bounds(current_index), error::wallet_internal_error, "Index out of bounds of hashchain");

  //Force processing of genesis transaction
  if ((m_blockchain.size() == 1) && (start_height == 0)) {
    cryptonote::block genesis;
    generate_genesis(genesis);

    if (m_blockchain[0] == get_block_hash(genesis)) {
      LOG_PRINT_L2("Processing genesis transaction: " << string_tools::pod_to_hex(get_transaction_hash(genesis.miner_tx)));
      std::vector<uint64_t> o_indices_genesis = {0}; //genesis transaction output
      process_new_transaction(get_transaction_hash(genesis.miner_tx), genesis.miner_tx, o_indices_genesis, 0, genesis.timestamp, true, false, false);
    } else {
      LOG_ERROR("Skip processing of genesis transaction, genesis block hash does not match: " << string_tools::pod_to_hex(get_block_hash(genesis)));
    }
  }

  tools::threadpool& tpool = tools::threadpool::getInstance();
  int threads = tpool.get_max_concurrency();
  if (threads > 1)
  {
    std::vector<crypto::hash> round_block_hashes(threads);
    std::vector<cryptonote::block> round_blocks(threads);
    std::deque<bool> error(threads);
    size_t blocks_size = blocks.size();
    std::list<block_complete_entry>::const_iterator blocki = blocks.begin();
    for (size_t b = 0; b < blocks_size; b += threads)
    {
      size_t round_size = std::min((size_t)threads, blocks_size - b);
      tools::threadpool::waiter waiter;

      std::list<block_complete_entry>::const_iterator tmpblocki = blocki;
      for (size_t i = 0; i < round_size; ++i)
      {
        tpool.submit(&waiter, boost::bind(&wallet::parse_block_round, this, std::cref(tmpblocki->block),
          std::ref(round_blocks[i]), std::ref(round_block_hashes[i]), std::ref(error[i])));
        ++tmpblocki;
      }
      waiter.wait();
      tmpblocki = blocki;
      for (size_t i = 0; i < round_size; ++i)
      {
        THROW_WALLET_EXCEPTION_IF(error[i], error::block_parse_error, tmpblocki->block);
        ++tmpblocki;
      }
      for (size_t i = 0; i < round_size; ++i)
      {
        const crypto::hash &bl_id = round_block_hashes[i];
        cryptonote::block &bl = round_blocks[i];

        if(current_index >= m_blockchain.size())
        {
          process_new_blockchain_entry(bl, *blocki, bl_id, current_index, o_indices[b+i]);
          ++blocks_added;
        }
        else if(bl_id != m_blockchain[current_index])
        {
          //split detected here !!!
          THROW_WALLET_EXCEPTION_IF(current_index == start_height, error::wallet_internal_error,
            "wrong daemon response: split starts from the first block in response " + string_tools::pod_to_hex(bl_id) +
            " (height " + std::to_string(start_height) + "), local block id at this height: " +
            string_tools::pod_to_hex(m_blockchain[current_index]));

          detach_blockchain(current_index);
          process_new_blockchain_entry(bl, *blocki, bl_id, current_index, o_indices[b+i]);
        }
        else
        {
          LOG_PRINT_L2("Block is already in blockchain: " << string_tools::pod_to_hex(bl_id));
        }
        ++current_index;
        ++blocki;
      }
    }
  }
  else
  {
  for(auto& bl_entry: blocks)
  {
    cryptonote::block bl;
    bool r = cryptonote::parse_and_validate_block_from_blob(bl_entry.block, bl);
    THROW_WALLET_EXCEPTION_IF(!r, error::block_parse_error, bl_entry.block);

    crypto::hash bl_id = get_block_hash(bl);
    if(current_index >= m_blockchain.size())
    {
      process_new_blockchain_entry(bl, bl_entry, bl_id, current_index, o_indices[tx_o_indices_idx]);
      ++blocks_added;
    }
    else if(bl_id != m_blockchain[current_index])
    {
      //split detected here !!!
      THROW_WALLET_EXCEPTION_IF(current_index == start_height, error::wallet_internal_error,
        "wrong daemon response: split starts from the first block in response " + string_tools::pod_to_hex(bl_id) +
        " (height " + std::to_string(start_height) + "), local block id at this height: " +
        string_tools::pod_to_hex(m_blockchain[current_index]));

      detach_blockchain(current_index);
      process_new_blockchain_entry(bl, bl_entry, bl_id, current_index, o_indices[tx_o_indices_idx]);
    }
    else
    {
      LOG_PRINT_L2("Block is already in blockchain: " << string_tools::pod_to_hex(bl_id));
    }

    ++current_index;
    ++tx_o_indices_idx;
  }
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::refresh()
{
  uint64_t blocks_fetched = 0;
  refresh(0, blocks_fetched);
}
//----------------------------------------------------------------------------------------------------
void wallet::refresh(uint64_t start_height, uint64_t & blocks_fetched)
{
  bool received_money = false;
  refresh(start_height, blocks_fetched, received_money);
}
//----------------------------------------------------------------------------------------------------
void wallet::pull_next_blocks(uint64_t start_height, uint64_t &blocks_start_height, std::list<crypto::hash> &short_chain_history, const std::list<cryptonote::block_complete_entry> &prev_blocks, std::list<cryptonote::block_complete_entry> &blocks, std::vector<cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::block_output_indices> &o_indices, bool &error)
{
  error = false;

  try
  {
    drop_from_short_history(short_chain_history, 3);

    // prepend the last 3 blocks, should be enough to guard against a block or two's reorg
    cryptonote::block bl;
    std::list<cryptonote::block_complete_entry>::const_reverse_iterator i = prev_blocks.rbegin();
    for (size_t n = 0; n < std::min((size_t)3, prev_blocks.size()); ++n)
    {
      bool ok = cryptonote::parse_and_validate_block_from_blob(i->block, bl);
      THROW_WALLET_EXCEPTION_IF(!ok, error::block_parse_error, i->block);
      short_chain_history.push_front(cryptonote::get_block_hash(bl));
      ++i;
    }

    // pull the new blocks
    pull_blocks(start_height, blocks_start_height, short_chain_history, blocks, o_indices);
  }
  catch(...)
  {
    error = true;
  }
}

void wallet::remove_obsolete_pool_txs(const std::vector<crypto::hash> &tx_hashes)
{
  // remove pool txes to us that aren't in the pool anymore
  std::unordered_multimap<crypto::hash, wallet::pool_payment_details>::iterator uit = m_unconfirmed_payments.begin();
  while (uit != m_unconfirmed_payments.end())
  {
    const crypto::hash &txid = uit->second.m_pd.m_tx_hash;
    bool found = false;
    for (const auto &it2: tx_hashes)
    {
      if (it2 == txid)
      {
        found = true;
        break;
      }
    }
    auto pit = uit++;
    if (!found)
    {
      MDEBUG("Removing " << txid << " from unconfirmed payments, not found in pool");
      m_unconfirmed_payments.erase(pit);
      if (0 != m_callback)
        m_callback->on_pool_tx_removed(txid);
    }
  }
}

//----------------------------------------------------------------------------------------------------
void wallet::update_pool_state(bool refreshed)
{
  MDEBUG("update_pool_state start");

  // get the pool state
  cryptonote::COMMAND_RPC_GET_TRANSACTION_POOL_HASHES::request req;
  cryptonote::COMMAND_RPC_GET_TRANSACTION_POOL_HASHES::response res;
  m_daemon_rpc_mutex.lock();
  bool r = epee::net_utils::invoke_http_json("/get_transaction_pool_hashes.bin", req, res, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_transaction_pool_hashes.bin");
  THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_transaction_pool_hashes.bin");
  THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::get_tx_pool_error);
  MDEBUG("update_pool_state got pool");

  // remove any pending tx that's not in the pool
  std::unordered_map<crypto::hash, wallet::unconfirmed_transfer_details>::iterator it = m_unconfirmed_txs.begin();
  while (it != m_unconfirmed_txs.end())
  {
    const crypto::hash &txid = it->first;
    bool found = false;
    for (const auto &it2: res.tx_hashes)
    {
      if (it2 == txid)
      {
        found = true;
        break;
      }
    }
    auto pit = it++;
    if (!found)
    {
      // we want to avoid a false positive when we ask for the pool just after
      // a tx is removed from the pool due to being found in a new block, but
      // just before the block is visible by refresh. So we keep a boolean, so
      // that the first time we don't see the tx, we set that boolean, and only
      // delete it the second time it is checked (but only when refreshed, so
      // we're sure we've seen the blockchain state first)
      if (pit->second.m_state == wallet::unconfirmed_transfer_details::pending)
      {
        LOG_PRINT_L1("Pending txid " << txid << " not in pool, marking as not in pool");
        pit->second.m_state = wallet::unconfirmed_transfer_details::pending_not_in_pool;
      }
      else if (pit->second.m_state == wallet::unconfirmed_transfer_details::pending_not_in_pool && refreshed)
      {
        LOG_PRINT_L1("Pending txid " << txid << " not in pool, marking as failed");
        pit->second.m_state = wallet::unconfirmed_transfer_details::failed;

        // the inputs aren't spent anymore, since the tx failed
        remove_rings(pit->second.m_tx);
        for (size_t vini = 0; vini < pit->second.m_tx.vin.size(); ++vini)
        {
          if (pit->second.m_tx.vin[vini].type() == typeid(txin_to_key))
          {
            txin_to_key &tx_in_to_key = boost::get<txin_to_key>(pit->second.m_tx.vin[vini]);
            for (size_t i = 0; i < m_transfers.size(); ++i)
            {
              const transfer_details &td = m_transfers[i];
              if (td.m_key_image == tx_in_to_key.k_image)
              {
                 LOG_PRINT_L1("Resetting spent status for output " << vini << ": " << td.m_key_image);
                 set_unspent(i);
                 break;
              }
            }
          }
        }
      }
    }
  }
  MDEBUG("update_pool_state done first loop");

  // remove pool txes to us that aren't in the pool anymore
  // but only if we just refreshed, so that the tx can go in
  // the in transfers list instead (or nowhere if it just
  // disappeared without being mined)
  if (refreshed)
    remove_obsolete_pool_txs(res.tx_hashes);

  MDEBUG("update_pool_state done second loop");

  // gather txids of new pool txes to us
  std::vector<std::pair<crypto::hash, bool>> txids;
  for (const auto &txid: res.tx_hashes)
  {
    bool txid_found_in_up = false;
    for (const auto &up: m_unconfirmed_payments)
    {
      if (up.second.m_pd.m_tx_hash == txid)
      {
        txid_found_in_up = true;
        break;
      }
    }
    if (m_scanned_pool_txs[0].find(txid) != m_scanned_pool_txs[0].end() || m_scanned_pool_txs[1].find(txid) != m_scanned_pool_txs[1].end())
    {
      // if it's for us, we want to keep track of whether we saw a double spend, so don't bail out
      if (!txid_found_in_up)
      {
        LOG_PRINT_L2("Already seen " << txid << ", and not for us, skipped");
        continue;
      }
    }
    if (!txid_found_in_up)
    {
      LOG_PRINT_L1("Found new pool tx: " << txid);
      bool found = false;
      for (const auto &i: m_unconfirmed_txs)
      {
        if (i.first == txid)
        {
          found = true;
          // if this is a payment to yourself at a different subaddress account, don't skip it
          // so that you can see the incoming pool tx with 'show_transfers' on that receiving subaddress account
          const unconfirmed_transfer_details& utd = i.second;
          for (const auto& dst : utd.m_dests)
          {
            auto subaddr_index = m_subaddresses.find(dst.addr.m_spend_public_key);
            if (subaddr_index != m_subaddresses.end() && subaddr_index->second.major != utd.m_subaddr_account)
            {
              found = false;
              break;
            }
          }
          break;
        }
      }
      if (!found)
      {
        // not one of those we sent ourselves
        txids.push_back({txid, false});
      }
      else
      {
        LOG_PRINT_L1("We sent that one");
      }
    }
    else
    {
      LOG_PRINT_L1("Already saw that one, it's for us");
      txids.push_back({txid, true});
    }
  }

  // get those txes
  if (!txids.empty())
  {
    cryptonote::COMMAND_RPC_GET_TRANSACTIONS::request req;
    cryptonote::COMMAND_RPC_GET_TRANSACTIONS::response res;
    for (const auto &p: txids)
      req.txs_hashes.push_back(epee::string_tools::pod_to_hex(p.first));
    MDEBUG("asking for " << txids.size() << " transactions");
    req.decode_as_json = false;
    req.prune = false;
    m_daemon_rpc_mutex.lock();
    bool r = epee::net_utils::invoke_http_json("/gettransactions", req, res, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    MDEBUG("Got " << r << " and " << res.status);
    if (r && res.status == CORE_RPC_STATUS_OK)
    {
      if (res.txs.size() == txids.size())
      {
        for (const auto &tx_entry: res.txs)
        {
          if (tx_entry.in_pool)
          {
            cryptonote::transaction tx;
            cryptonote::blobdata bd;
            crypto::hash tx_hash, tx_prefix_hash;
            if (epee::string_tools::parse_hexstr_to_binbuff(tx_entry.as_hex, bd))
            {
              if (cryptonote::parse_and_validate_tx_from_blob(bd, tx, tx_hash, tx_prefix_hash))
              {
                const std::vector<std::pair<crypto::hash, bool>>::const_iterator i = std::find_if(txids.begin(), txids.end(),
                    [tx_hash](const std::pair<crypto::hash, bool> &e) { return e.first == tx_hash; });
                if (i != txids.end())
                {
                  process_new_transaction(tx_hash, tx, std::vector<uint64_t>(), 0, time(NULL), false, true, tx_entry.double_spend_seen);
                  m_scanned_pool_txs[0].insert(tx_hash);
                  if (m_scanned_pool_txs[0].size() > 5000)
                  {
                    std::swap(m_scanned_pool_txs[0], m_scanned_pool_txs[1]);
                    m_scanned_pool_txs[0].clear();
                  }
                }
                else
                {
                  MERROR("Got txid " << tx_hash << " which we did not ask for");
                }
              }
              else
              {
                LOG_PRINT_L0("failed to validate transaction from daemon");
              }
            }
            else
            {
              LOG_PRINT_L0("Failed to parse transaction from daemon");
            }
          }
          else
          {
            LOG_PRINT_L1("Transaction from daemon was in pool, but is no more");
          }
        }
      }
      else
      {
        LOG_PRINT_L0("Expected " << txids.size() << " tx(es), got " << res.txs.size());
      }
    }
    else
    {
      LOG_PRINT_L0("Error calling gettransactions daemon RPC: r " << r << ", status " << res.status);
    }
  }
  MDEBUG("update_pool_state end");
}
//----------------------------------------------------------------------------------------------------
void wallet::fast_refresh(uint64_t stop_height, uint64_t &blocks_start_height, std::list<crypto::hash> &short_chain_history)
{
  std::list<crypto::hash> hashes;

  const uint64_t checkpoint_height = m_checkpoints.get_max_height();
  if (stop_height > checkpoint_height && m_blockchain.size()-1 < checkpoint_height)
  {
    // we will drop all these, so don't bother getting them
    uint64_t missing_blocks = m_checkpoints.get_max_height() - m_blockchain.size();
    while (missing_blocks-- > 0)
      m_blockchain.push_back(crypto::null_hash); // maybe a bit suboptimal, but deque won't do huge reallocs like vector
    m_blockchain.push_back(m_checkpoints.get_points().at(checkpoint_height));
    m_local_bc_height = m_blockchain.size();
    short_chain_history.clear();
    get_short_chain_history(short_chain_history);
  }

  size_t current_index = m_blockchain.size();
  while(m_run.load(std::memory_order_relaxed) && current_index < stop_height)
  {
    pull_hashes(0, blocks_start_height, short_chain_history, hashes);
    if (hashes.size() <= 3)
      return;
    if (blocks_start_height < m_blockchain.offset())
    {
      MERROR("Blocks start before blockchain offset: " << blocks_start_height << " " << m_blockchain.offset());
      return;
    }
    if (hashes.size() + current_index < stop_height) {
      drop_from_short_history(short_chain_history, 3);
      std::list<crypto::hash>::iterator right = hashes.end();
      // prepend 3 more
      for (int i = 0; i<3; i++) {
        right--;
        short_chain_history.push_front(*right);
      }
    }
    current_index = blocks_start_height;
    for(auto& bl_id: hashes)
    {
      if(current_index >= m_blockchain.size())
      {
        if (!(current_index % 1000))
          LOG_PRINT_L2( "Skipped block by height: " << current_index);
        m_blockchain.push_back(bl_id);
        ++m_local_bc_height;

        if (0 != m_callback)
        { // FIXME: this isn't right, but simplewallet just logs that we got a block.
          cryptonote::block dummy;
          m_callback->on_new_block(current_index, dummy);
        }
      }
      else if(bl_id != m_blockchain[current_index])
      {
        //split detected here !!!
        return;
      }
      ++current_index;
      if (current_index >= stop_height)
        return;
    }
  }
}


bool wallet::add_address_book_row(const cryptonote::account_public_address &address, const crypto::hash &payment_id, const std::string &description, bool is_subaddress)
{
  wallet::address_book_row a;
  a.m_address = address;
  a.m_payment_id = payment_id;
  a.m_description = description;
  a.m_is_subaddress = is_subaddress;

  auto old_size = m_address_book.size();
  m_address_book.push_back(a);
  if(m_address_book.size() == old_size+1)
    return true;
  return false;
}

bool wallet::delete_address_book_row(std::size_t row_id) {
  if(m_address_book.size() <= row_id)
    return false;

  m_address_book.erase(m_address_book.begin()+row_id);

  return true;
}

//----------------------------------------------------------------------------------------------------
void wallet::refresh(uint64_t start_height, uint64_t & blocks_fetched, bool& received_money)
{
  if(m_light_wallet) {

    // MyMonero get_address_info needs to be called occasionally to trigger wallet sync.
    // This call is not really needed for other purposes and can be removed if mymonero changes their backend.
    cryptonote::COMMAND_RPC_GET_ADDRESS_INFO::response res;

    // Get basic info
    if(light_wallet_get_address_info(res)) {
      // Last stored block height
      uint64_t prev_height = m_light_wallet_blockchain_height;
      // Update lw heights
      m_light_wallet_scanned_block_height = res.scanned_block_height;
      m_light_wallet_blockchain_height = res.blockchain_height;
      m_local_bc_height = res.blockchain_height;
      // If new height - call new_block callback
      if(m_light_wallet_blockchain_height != prev_height)
      {
        MDEBUG("new block since last time!");
        m_callback->on_lw_new_block(m_light_wallet_blockchain_height - 1);
      }
      m_light_wallet_connected = true;
      MDEBUG("lw scanned block height: " <<  m_light_wallet_scanned_block_height);
      MDEBUG("lw blockchain height: " <<  m_light_wallet_blockchain_height);
      MDEBUG(m_light_wallet_blockchain_height-m_light_wallet_scanned_block_height << " blocks behind");
      // TODO: add wallet created block info

      light_wallet_get_address_txs();
    } else
      m_light_wallet_connected = false;

    // Lighwallet refresh done
    return;
  }
  received_money = false;
  blocks_fetched = 0;
  uint64_t added_blocks = 0;
  size_t try_count = 0;
  crypto::hash last_tx_hash_id = m_transfers.size() ? m_transfers.back().m_txid : null_hash;
  std::list<crypto::hash> short_chain_history;
  tools::threadpool& tpool = tools::threadpool::getInstance();
  tools::threadpool::waiter waiter;
  uint64_t blocks_start_height;
  std::list<cryptonote::block_complete_entry> blocks;
  std::vector<COMMAND_RPC_GET_BLOCKS_FAST::block_output_indices> o_indices;
  bool refreshed = false;

  // pull the first set of blocks
  get_short_chain_history(short_chain_history);
  m_run.store(true, std::memory_order_relaxed);
  if (start_height > m_blockchain.size() || m_refresh_from_block_height > m_blockchain.size()) {
    if (!start_height)
      start_height = m_refresh_from_block_height;
    // we can shortcut by only pulling hashes up to the start_height
    fast_refresh(start_height, blocks_start_height, short_chain_history);
    // regenerate the history now that we've got a full set of hashes
    short_chain_history.clear();
    get_short_chain_history(short_chain_history);
    start_height = 0;
    // and then fall through to regular refresh processing
  }

  // If stop() is called during fast refresh we don't need to continue
  if(!m_run.load(std::memory_order_relaxed))
    return;
  pull_blocks(start_height, blocks_start_height, short_chain_history, blocks, o_indices);
  // always reset start_height to 0 to force short_chain_ history to be used on
  // subsequent pulls in this refresh.
  start_height = 0;

  while(m_run.load(std::memory_order_relaxed))
  {
    try
    {
      // pull the next set of blocks while we're processing the current one
      uint64_t next_blocks_start_height;
      std::list<cryptonote::block_complete_entry> next_blocks;
      std::vector<cryptonote::COMMAND_RPC_GET_BLOCKS_FAST::block_output_indices> next_o_indices;
      bool error = false;
      if (blocks.empty())
      {
        refreshed = false;
        break;
      }
      tpool.submit(&waiter, [&]{pull_next_blocks(start_height, next_blocks_start_height, short_chain_history, blocks, next_blocks, next_o_indices, error);});

      process_blocks(blocks_start_height, blocks, o_indices, added_blocks);
      blocks_fetched += added_blocks;
      waiter.wait();
      if(blocks_start_height == next_blocks_start_height)
      {
        m_node_rpc_proxy.set_height(m_blockchain.size());
        refreshed = true;
        break;
      }

      // switch to the new blocks from the daemon
      blocks_start_height = next_blocks_start_height;
      blocks = next_blocks;
      o_indices = next_o_indices;

      // handle error from async fetching thread
      if (error)
      {
        throw std::runtime_error("proxy exception in refresh thread");
      }
    }
    catch (const std::exception&)
    {
      blocks_fetched += added_blocks;
      waiter.wait();
      if(try_count < 3)
      {
        LOG_PRINT_L1("Another try pull_blocks (try_count=" << try_count << ")...");
        ++try_count;
      }
      else
      {
        LOG_ERROR("pull_blocks failed, try_count=" << try_count);
        throw;
      }
    }
  }
  if(last_tx_hash_id != (m_transfers.size() ? m_transfers.back().m_txid : null_hash))
    received_money = true;

  try
  {
    // If stop() is called we don't need to check pending transactions
    if(m_run.load(std::memory_order_relaxed))
      update_pool_state(refreshed);
  }
  catch (...)
  {
    LOG_PRINT_L1("Failed to check pending transactions");
  }

  LOG_PRINT_L1("Refresh done, blocks received: " << blocks_fetched << ", balance (all accounts): " << print_money(balance_all()) << ", unlocked: " << print_money(unlocked_balance_all()));
}
//----------------------------------------------------------------------------------------------------
bool wallet::refresh(uint64_t & blocks_fetched, bool& received_money, bool& ok)
{
  try
  {
    refresh(0, blocks_fetched, received_money);
    ok = true;
  }
  catch (...)
  {
    ok = false;
  }
  return ok;
}
//----------------------------------------------------------------------------------------------------
bool wallet::get_output_distribution(uint64_t &start_height, std::vector<uint64_t> &distribution)
{
  uint32_t rpc_version;
  boost::optional<std::string> result = m_node_rpc_proxy.get_rpc_version(rpc_version);
  // no error
  if (!!result)
  {
    // empty string -> not connection
    THROW_WALLET_EXCEPTION_IF(result->empty(), tools::error::no_connection_to_daemon, "getversion");
    THROW_WALLET_EXCEPTION_IF(*result == CORE_RPC_STATUS_BUSY, tools::error::daemon_busy, "getversion");
    if (*result != CORE_RPC_STATUS_OK)
    {
      MDEBUG("Cannot determine daemon RPC version, not requesting rct distribution");
      return false;
    }
  }
  else
  {
    if (rpc_version >= MAKE_CORE_RPC_VERSION(1, 19))
    {
      MDEBUG("Daemon is recent enough, requesting rct distribution");
    }
    else
    {
      MDEBUG("Daemon is too old, not requesting rct distribution");
      return false;
    }
  }

  cryptonote::COMMAND_RPC_GET_OUTPUT_DISTRIBUTION::request req = AUTO_VAL_INIT(req);
  cryptonote::COMMAND_RPC_GET_OUTPUT_DISTRIBUTION::response res = AUTO_VAL_INIT(res);
  req.amounts.push_back(0);
  req.from_height = 0;
  req.cumulative = true;
  m_daemon_rpc_mutex.lock();
  bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_output_distribution", req, res, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  if (!r)
  {
    MWARNING("Failed to request output distribution: no connection to daemon");
    return false;
  }
  if (res.status == CORE_RPC_STATUS_BUSY)
  {
    MWARNING("Failed to request output distribution: daemon is busy");
    return false;
  }
  if (res.status != CORE_RPC_STATUS_OK)
  {
    MWARNING("Failed to request output distribution: " << res.status);
    return false;
  }
  if (res.distributions.size() != 1)
  {
    MWARNING("Failed to request output distribution: not the expected single result");
    return false;
  }
  if (res.distributions[0].amount != 0)
  {
    MWARNING("Failed to request output distribution: results are not for amount 0");
    return false;
  }
  start_height = res.distributions[0].start_height;
  distribution = std::move(res.distributions[0].distribution);
  return true;
}
//----------------------------------------------------------------------------------------------------
void wallet::detach_blockchain(uint64_t height)
{
  LOG_PRINT_L0("Detaching blockchain on height " << height);

  // size  1 2 3 4 5 6 7 8 9
  // block 0 1 2 3 4 5 6 7 8
  //               C
  THROW_WALLET_EXCEPTION_IF(height < m_blockchain.offset() && m_blockchain.size() > m_blockchain.offset(),
      error::wallet_internal_error, "Daemon claims reorg below last checkpoint");

  size_t transfers_detached = 0;

  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    wallet::transfer_details &td = m_transfers[i];
    if (td.m_spent && td.m_spent_height >= height)
    {
      LOG_PRINT_L1("Resetting spent status for output " << i << ": " << td.m_key_image);
      set_unspent(i);
    }
  }

  auto it = std::find_if(m_transfers.begin(), m_transfers.end(), [&](const transfer_details& td){return td.m_block_height >= height;});
  size_t i_start = it - m_transfers.begin();

  for(size_t i = i_start; i!= m_transfers.size();i++)
  {
    if (!m_transfers[i].m_key_image_known || m_transfers[i].m_key_image_partial)
      continue;
    auto it_ki = m_key_images.find(m_transfers[i].m_key_image);
    THROW_WALLET_EXCEPTION_IF(it_ki == m_key_images.end(), error::wallet_internal_error, "key image not found: index " + std::to_string(i) + ", ki " + epee::string_tools::pod_to_hex(m_transfers[i].m_key_image) + ", " + std::to_string(m_key_images.size()) + " key images known");
    m_key_images.erase(it_ki);
  }

  for(size_t i = i_start; i!= m_transfers.size();i++)
  {
    auto it_pk = m_pub_keys.find(m_transfers[i].get_public_key());
    THROW_WALLET_EXCEPTION_IF(it_pk == m_pub_keys.end(), error::wallet_internal_error, "public key not found");
    m_pub_keys.erase(it_pk);
  }
  m_transfers.erase(it, m_transfers.end());

  size_t blocks_detached = m_blockchain.size() - height;
  m_blockchain.crop(height);
  m_local_bc_height -= blocks_detached;

  for (auto it = m_payments.begin(); it != m_payments.end(); )
  {
    if(height <= it->second.m_block_height)
      it = m_payments.erase(it);
    else
      ++it;
  }

  for (auto it = m_confirmed_txs.begin(); it != m_confirmed_txs.end(); )
  {
    if(height <= it->second.m_block_height)
      it = m_confirmed_txs.erase(it);
    else
      ++it;
  }

  LOG_PRINT_L0("Detached blockchain on height " << height << ", transfers detached " << transfers_detached << ", blocks detached " << blocks_detached);
}
//----------------------------------------------------------------------------------------------------
bool wallet::deinit()
{
  m_is_initialized=false;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::clear()
{
  m_blockchain.clear();
  m_transfers.clear();
  m_key_images.clear();
  m_pub_keys.clear();
  m_unconfirmed_txs.clear();
  m_payments.clear();
  m_tx_keys.clear();
  m_additional_tx_keys.clear();
  m_confirmed_txs.clear();
  m_unconfirmed_payments.clear();
  m_scanned_pool_txs[0].clear();
  m_scanned_pool_txs[1].clear();
  m_address_book.clear();
  m_local_bc_height = 1;
  m_subaddresses.clear();
  m_subaddress_labels.clear();
  return true;
}

/*!
 * \brief Stores wallet information to wallet file.
 * \param  keys_file_name Name of wallet file
 * \param  password       Password of wallet file
 * \param  watch_only     true to save only view key, false to save both spend and view keys
 * \return                Whether it was successful.
 */
bool wallet::store_keys(const std::string& keys_file_name, const epee::wipeable_string& password, bool watch_only)
{
  std::string account_data;
  std::string multisig_signers;
  cryptonote::account_base account = m_account;

  if (watch_only)
    account.forget_spend_key();
  bool r = epee::serialization::store_t_to_binary(account, account_data);
  CHECK_AND_ASSERT_MES(r, false, "failed to serialize wallet keys");
  wallet::keys_file_data keys_file_data = boost::value_initialized<wallet::keys_file_data>();

  // Create a JSON object with "key_data" and "seed_language" as keys.
  rapidjson::Document json;
  json.SetObject();
  rapidjson::Value value(rapidjson::kStringType);
  value.SetString(account_data.c_str(), account_data.length());
  json.AddMember("key_data", value, json.GetAllocator());
  if (!seed_language.empty())
  {
    value.SetString(seed_language.c_str(), seed_language.length());
    json.AddMember("seed_language", value, json.GetAllocator());
  }

  rapidjson::Value value2(rapidjson::kNumberType);

  value2.SetInt(m_key_on_device?1:0);
  json.AddMember("key_on_device", value2, json.GetAllocator());

  value2.SetInt(watch_only ? 1 :0); // WTF ? JSON has different true and false types, and not boolean ??
  json.AddMember("watch_only", value2, json.GetAllocator());

  value2.SetInt(m_multisig ? 1 :0);
  json.AddMember("multisig", value2, json.GetAllocator());

  value2.SetUint(m_multisig_threshold);
  json.AddMember("multisig_threshold", value2, json.GetAllocator());


  value2.SetInt(m_always_confirm_transfers ? 1 :0);
  json.AddMember("always_confirm_transfers", value2, json.GetAllocator());

  value2.SetInt(m_print_ring_members ? 1 :0);
  json.AddMember("print_ring_members", value2, json.GetAllocator());

  value2.SetInt(m_store_tx_info ? 1 :0);
  json.AddMember("store_tx_info", value2, json.GetAllocator());

  value2.SetUint(m_default_mixin);
  json.AddMember("default_mixin", value2, json.GetAllocator());

  value2.SetUint(m_default_priority);
  json.AddMember("default_priority", value2, json.GetAllocator());

  value2.SetInt(m_auto_refresh ? 1 :0);
  json.AddMember("auto_refresh", value2, json.GetAllocator());

  value2.SetInt(m_refresh_type);
  json.AddMember("refresh_type", value2, json.GetAllocator());

  value2.SetUint64(m_refresh_from_block_height);
  json.AddMember("refresh_height", value2, json.GetAllocator());

  value2.SetInt(m_confirm_missing_payment_id ? 1 :0);
  json.AddMember("confirm_missing_payment_id", value2, json.GetAllocator());

  value2.SetInt(m_confirm_non_default_ring_size ? 1 :0);
  json.AddMember("confirm_non_default_ring_size", value2, json.GetAllocator());

  value2.SetInt(m_ask_password ? 1 :0);
  json.AddMember("ask_password", value2, json.GetAllocator());

  value2.SetUint(m_min_output_count);
  json.AddMember("min_output_count", value2, json.GetAllocator());

  value2.SetUint64(m_min_output_value);
  json.AddMember("min_output_value", value2, json.GetAllocator());

  value2.SetInt(cryptonote::get_default_decimal_point());
  json.AddMember("default_decimal_point", value2, json.GetAllocator());

  value2.SetInt(m_merge_destinations ? 1 :0);
  json.AddMember("merge_destinations", value2, json.GetAllocator());

  value2.SetInt(m_confirm_backlog ? 1 :0);
  json.AddMember("confirm_backlog", value2, json.GetAllocator());

  value2.SetUint(m_confirm_backlog_threshold);
  json.AddMember("confirm_backlog_threshold", value2, json.GetAllocator());

  value2.SetInt(m_confirm_export_overwrite ? 1 :0);
  json.AddMember("confirm_export_overwrite", value2, json.GetAllocator());

  value2.SetInt(m_auto_low_priority ? 1 : 0);
  json.AddMember("auto_low_priority", value2, json.GetAllocator());

  value2.SetUint(m_nettype);
  json.AddMember("nettype", value2, json.GetAllocator());

  value2.SetInt(m_segregate_pre_fork_outputs ? 1 : 0);
  json.AddMember("segregate_pre_fork_outputs", value2, json.GetAllocator());

  value2.SetInt(m_key_reuse_mitigation2 ? 1 : 0);
  json.AddMember("key_reuse_mitigation2", value2, json.GetAllocator());

  value2.SetUint(m_segregation_height);
  json.AddMember("segregation_height", value2, json.GetAllocator());

  value2.SetUint(m_subaddress_lookahead_major);
  json.AddMember("subaddress_lookahead_major", value2, json.GetAllocator());

  value2.SetUint(m_subaddress_lookahead_minor);
  json.AddMember("subaddress_lookahead_minor", value2, json.GetAllocator());

  // Serialize the JSON object
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  json.Accept(writer);
  account_data = buffer.GetString();

  // Encrypt the entire JSON object.
  crypto::chacha_key key;
  crypto::generate_chacha_key(password.data(), password.size(), key);
  std::string cipher;
  cipher.resize(account_data.size());
  keys_file_data.iv = crypto::rand<crypto::chacha_iv>();
  crypto::chacha20(account_data.data(), account_data.size(), key, keys_file_data.iv, &cipher[0]);
  keys_file_data.account_data = cipher;

  std::string buf;
  r = ::serialization::dump_binary(keys_file_data, buf);
  r = r && epee::file_io_utils::save_string_to_file(keys_file_name, buf); //and never touch wallet_keys_file again, only read
  CHECK_AND_ASSERT_MES(r, false, "failed to generate wallet keys file " << keys_file_name);

  return true;
}
//----------------------------------------------------------------------------------------------------
/*!
 * \brief Load wallet information from wallet file.
 * \param keys_file_name Name of wallet file
 * \param password       Password of wallet file
 */
bool wallet::load_keys(const std::string& keys_file_name, const epee::wipeable_string& password)
{
  rapidjson::Document json;
  wallet::keys_file_data keys_file_data;
  std::string buf;
  bool r = epee::file_io_utils::load_file_to_string(keys_file_name, buf);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_read_error, keys_file_name);

  // Decrypt the contents
  r = ::serialization::parse_binary(buf, keys_file_data);
  THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "internal error: failed to deserialize \"" + keys_file_name + '\"');
  crypto::chacha_key key;
  crypto::generate_chacha_key(password.data(), password.size(), key);
  std::string account_data;
  account_data.resize(keys_file_data.account_data.size());
  crypto::chacha20(keys_file_data.account_data.data(), keys_file_data.account_data.size(), key, keys_file_data.iv, &account_data[0]);
  if (json.Parse(account_data.c_str()).HasParseError() || !json.IsObject())
    crypto::chacha8(keys_file_data.account_data.data(), keys_file_data.account_data.size(), key, keys_file_data.iv, &account_data[0]);

  // The contents should be JSON if the wallet follows the new format.
  if (json.Parse(account_data.c_str()).HasParseError())
  {
    is_old_file_format = true;
    m_watch_only = false;
    m_multisig = false;
    m_multisig_threshold = 0;
    m_multisig_signers.clear();
    m_always_confirm_transfers = false;
    m_print_ring_members = false;
    m_default_mixin = 0;
    m_default_priority = 0;
    m_auto_refresh = true;
    m_refresh_type = RefreshType::RefreshDefault;
    m_confirm_missing_payment_id = true;
    m_confirm_non_default_ring_size = true;
    m_ask_password = true;
    m_min_output_count = 0;
    m_min_output_value = 0;
    m_merge_destinations = false;
    m_confirm_backlog = true;
    m_confirm_backlog_threshold = 0;
    m_confirm_export_overwrite = true;
    m_auto_low_priority = true;
    m_segregate_pre_fork_outputs = true;
    m_key_reuse_mitigation2 = true;
    m_segregation_height = 0;
    m_subaddress_lookahead_major = SUBADDRESS_LOOKAHEAD_MAJOR;
    m_subaddress_lookahead_minor = SUBADDRESS_LOOKAHEAD_MINOR;
    m_key_on_device = false;
  }
  else if(json.IsObject())
  {
    if (!json.HasMember("key_data"))
    {
      LOG_ERROR("Field key_data not found in JSON");
      return false;
    }
    if (!json["key_data"].IsString())
    {
      LOG_ERROR("Field key_data found in JSON, but not String");
      return false;
    }
    const char *field_key_data = json["key_data"].GetString();
    account_data = std::string(field_key_data, field_key_data + json["key_data"].GetStringLength());

    if (json.HasMember("key_on_device"))
    {
      GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, key_on_device, int, Int, false, false);
      m_key_on_device = field_key_on_device;
    }

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, seed_language, std::string, String, false, std::string());
    if (field_seed_language_found)
    {
      set_seed_language(field_seed_language);
    }
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, watch_only, int, Int, false, false);
    m_watch_only = field_watch_only;

    m_multisig = false;
    m_multisig_threshold = 0;

    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, always_confirm_transfers, int, Int, false, true);
    m_always_confirm_transfers = field_always_confirm_transfers;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, print_ring_members, int, Int, false, true);
    m_print_ring_members = field_print_ring_members;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, store_tx_keys, int, Int, false, true);
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, store_tx_info, int, Int, false, true);
    m_store_tx_info = ((field_store_tx_keys != 0) || (field_store_tx_info != 0));
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, default_mixin, unsigned int, Uint, false, 0);
    m_default_mixin = field_default_mixin;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, default_priority, unsigned int, Uint, false, 0);
    if (field_default_priority_found)
    {
      m_default_priority = field_default_priority;
    }
    else
    {
      GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, default_fee_multiplier, unsigned int, Uint, false, 0);
      if (field_default_fee_multiplier_found)
        m_default_priority = field_default_fee_multiplier;
      else
        m_default_priority = 0;
    }
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, auto_refresh, int, Int, false, true);
    m_auto_refresh = field_auto_refresh;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, refresh_type, int, Int, false, RefreshType::RefreshDefault);
    m_refresh_type = RefreshType::RefreshDefault;
    if (field_refresh_type_found)
    {
      if (field_refresh_type == RefreshFull || field_refresh_type == RefreshOptimizeCoinbase || field_refresh_type == RefreshNoCoinbase)
        m_refresh_type = (RefreshType)field_refresh_type;
      else
        LOG_PRINT_L0("Unknown refresh-type value (" << field_refresh_type << "), using default");
    }
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, refresh_height, uint64_t, Uint64, false, 0);
    m_refresh_from_block_height = field_refresh_height;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, confirm_missing_payment_id, int, Int, false, true);
    m_confirm_missing_payment_id = field_confirm_missing_payment_id;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, confirm_non_default_ring_size, int, Int, false, true);
    m_confirm_non_default_ring_size = field_confirm_non_default_ring_size;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, ask_password, int, Int, false, true);
    m_ask_password = field_ask_password;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, default_decimal_point, int, Int, false, CRYPTONOTE_DISPLAY_DECIMAL_POINT);
    cryptonote::set_default_decimal_point(field_default_decimal_point);
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, min_output_count, uint32_t, Uint, false, 0);
    m_min_output_count = field_min_output_count;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, min_output_value, uint64_t, Uint64, false, 0);
    m_min_output_value = field_min_output_value;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, merge_destinations, int, Int, false, false);
    m_merge_destinations = field_merge_destinations;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, confirm_backlog, int, Int, false, true);
    m_confirm_backlog = field_confirm_backlog;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, confirm_backlog_threshold, uint32_t, Uint, false, 0);
    m_confirm_backlog_threshold = field_confirm_backlog_threshold;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, confirm_export_overwrite, int, Int, false, true);
    m_confirm_export_overwrite = field_confirm_export_overwrite;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, auto_low_priority, int, Int, false, true);
    m_auto_low_priority = field_auto_low_priority;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, nettype, uint8_t, Uint, false, static_cast<uint8_t>(m_nettype));
    // The network type given in the program argument is inconsistent with the network type saved in the wallet
    THROW_WALLET_EXCEPTION_IF(static_cast<uint8_t>(m_nettype) != field_nettype, error::wallet_internal_error,
    (boost::format("%s wallet cannot be opened as %s wallet")
    % (field_nettype == 0 ? "Mainnet" : field_nettype == 1 ? "Testnet" : "Stagenet")
    % (m_nettype == MAINNET ? "mainnet" : m_nettype == TESTNET ? "testnet" : "stagenet")).str());
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, segregate_pre_fork_outputs, int, Int, false, true);
    m_segregate_pre_fork_outputs = field_segregate_pre_fork_outputs;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, key_reuse_mitigation2, int, Int, false, true);
    m_key_reuse_mitigation2 = field_key_reuse_mitigation2;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, segregation_height, int, Uint, false, 0);
    m_segregation_height = field_segregation_height;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, subaddress_lookahead_major, uint32_t, Uint, false, SUBADDRESS_LOOKAHEAD_MAJOR);
    m_subaddress_lookahead_major = field_subaddress_lookahead_major;
    GET_FIELD_FROM_JSON_RETURN_ON_ERROR(json, subaddress_lookahead_minor, uint32_t, Uint, false, SUBADDRESS_LOOKAHEAD_MINOR);
    m_subaddress_lookahead_minor = field_subaddress_lookahead_minor;
  }
  else
  {
      THROW_WALLET_EXCEPTION(error::wallet_internal_error, "invalid password");
      return false;
  }

  r = epee::serialization::load_t_from_binary(m_account, account_data);
  if (r && m_key_on_device) {
    LOG_PRINT_L0("Account on device. Initing device...");
    hw::device &hwdev = hw::get_device("Ledger");
    hwdev.init();
    hwdev.connect();
    m_account.set_device(hwdev);
    LOG_PRINT_L0("Device inited...");
  }
  const cryptonote::account_keys& keys = m_account.get_keys();
  hw::device &hwdev = m_account.get_device();
  r = r && hwdev.verify_keys(keys.m_view_secret_key,  keys.m_account_address.m_view_public_key);
  if(!m_watch_only && !m_multisig)
    r = r && hwdev.verify_keys(keys.m_spend_secret_key, keys.m_account_address.m_spend_public_key);
  THROW_WALLET_EXCEPTION_IF(!r, error::invalid_password);
  return true;
}

/*!
 * \brief verify password for default wallet keys file.
 * \param password       Password to verify
 * \return               true if password is correct
 *
 * for verification only
 * should not mutate state, unlike load_keys()
 * can be used prior to rewriting wallet keys file, to ensure user has entered the correct password
 *
 */
bool wallet::verify_password(const epee::wipeable_string& password) const
{
  return verify_password(m_keys_file, password, m_watch_only, m_account.get_device());
}

/*!
 * \brief verify password for specified wallet keys file.
 * \param keys_file_name  Keys file to verify password for
 * \param password        Password to verify
 * \param no_spend_key    If set = only verify view keys, otherwise also spend keys
 * \return                true if password is correct
 *
 * for verification only
 * should not mutate state, unlike load_keys()
 * can be used prior to rewriting wallet keys file, to ensure user has entered the correct password
 *
 */
bool wallet::verify_password(const std::string& keys_file_name, const epee::wipeable_string& password, bool no_spend_key, hw::device &hwdev)
{
  rapidjson::Document json;
  wallet::keys_file_data keys_file_data;
  std::string buf;
  bool r = epee::file_io_utils::load_file_to_string(keys_file_name, buf);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_read_error, keys_file_name);

  // Decrypt the contents
  r = ::serialization::parse_binary(buf, keys_file_data);
  THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "internal error: failed to deserialize \"" + keys_file_name + '\"');
  crypto::chacha_key key;
  crypto::generate_chacha_key(password.data(), password.size(), key);
  std::string account_data;
  account_data.resize(keys_file_data.account_data.size());
  crypto::chacha20(keys_file_data.account_data.data(), keys_file_data.account_data.size(), key, keys_file_data.iv, &account_data[0]);
  if (json.Parse(account_data.c_str()).HasParseError() || !json.IsObject())
    crypto::chacha8(keys_file_data.account_data.data(), keys_file_data.account_data.size(), key, keys_file_data.iv, &account_data[0]);

  // The contents should be JSON if the wallet follows the new format.
  if (json.Parse(account_data.c_str()).HasParseError())
  {
    // old format before JSON wallet key file format
  }
  else
  {
    account_data = std::string(json["key_data"].GetString(), json["key_data"].GetString() +
      json["key_data"].GetStringLength());
  }

  cryptonote::account_base account_data_check;

  r = epee::serialization::load_t_from_binary(account_data_check, account_data);
  const cryptonote::account_keys& keys = account_data_check.get_keys();

  r = r && hwdev.verify_keys(keys.m_view_secret_key,  keys.m_account_address.m_view_public_key);
  if(!no_spend_key)
    r = r && hwdev.verify_keys(keys.m_spend_secret_key, keys.m_account_address.m_spend_public_key);
  return r;
}


/*!
 * \brief  Generates a wallet or restores one.
 * \param  wallet_        Name of wallet file
 * \param  password       Password of wallet file
 * \param  recovery_param If it is a restore, the recovery key
 * \param  recover        Whether it is a restore
 * \param  two_random     Whether it is a non-deterministic wallet
 * \return                The secret key of the generated wallet
 */
crypto::secret_key wallet::generate(const std::string& wallet_, const epee::wipeable_string& password,
  const crypto::secret_key& recovery_param, bool recover, bool two_random, bool create_address_file)
{
  clear();
  prepare_file_names(wallet_);

  if (!wallet_.empty())
  {
    boost::system::error_code ignored_ec;
    THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_wallet_file, ignored_ec), error::file_exists, m_wallet_file);
    THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_keys_file,   ignored_ec), error::file_exists, m_keys_file);
  }

  crypto::secret_key retval = m_account.generate(recovery_param, recover, two_random);

  m_account_public_address = m_account.get_keys().m_account_address;
  m_watch_only = false;
  m_multisig = false;
  m_multisig_threshold = 0;
  m_multisig_signers.clear();
  m_key_on_device = false;

  // calculate a starting refresh height
  if(m_refresh_from_block_height == 0 && !recover){
    m_refresh_from_block_height = estimate_blockchain_height();
  }

  if (!wallet_.empty())
  {
    bool r = store_keys(m_keys_file, password, false);
    THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);

    if (m_nettype != MAINNET || create_address_file)
    {
      r = file_io_utils::save_string_to_file(m_wallet_file + ".address.txt", m_account.get_public_address_str(m_nettype));
      if(!r) MERROR("String with address text not saved");
    }
  }

  cryptonote::block b = AUTO_VAL_INIT(b);
  generate_genesis(b);
  m_blockchain.push_back(get_block_hash(b));
  add_subaddress_account(tr("Primary account"));

  if (!wallet_.empty())
    store();

  return retval;
}

 uint64_t wallet::estimate_blockchain_height()
 {
   // -1 month for fluctuations in block time and machine date/time setup.
   // avg seconds per block
   const int seconds_per_block = DIFFICULTY_TARGET;
   // ~num blocks per month
   const uint64_t blocks_per_month = 60*60*24*30/seconds_per_block;

   // try asking the daemon first
   std::string err;
   uint64_t height = 0;

   // we get the max of approximated height and local height.
   // approximated height is the least of daemon target height
   // (the max of what the other daemons are claiming is their
   // height) and the theoretical height based on the local
   // clock. This will be wrong only if both the local clock
   // is bad *and* a peer daemon claims a highest height than
   // the real chain.
   // local height is the height the local daemon is currently
   // synced to, it will be lower than the real chain height if
   // the daemon is currently syncing.
   // If we use the approximate height we subtract one month as
   // a safety margin.
   height = get_approximate_blockchain_height();
   uint64_t target_height = get_daemon_blockchain_target_height(err);
   if (err.empty()) {
     if (target_height < height)
       height = target_height;
   } else {
     // if we couldn't talk to the daemon, check safety margin.
     if (height > blocks_per_month)
       height -= blocks_per_month;
     else
       height = 0;
   }
   uint64_t local_height = get_daemon_blockchain_height(err);
   if (err.empty() && local_height > height)
     height = local_height;
   return height;
 }

/*!
* \brief Creates a watch only wallet from a public address and a view secret key.
* \param  wallet_        Name of wallet file
* \param  password       Password of wallet file
* \param  viewkey        view secret key
*/
void wallet::generate(const std::string& wallet_, const epee::wipeable_string& password,
  const cryptonote::account_public_address &account_public_address,
  const crypto::secret_key& viewkey, bool create_address_file)
{
  clear();
  prepare_file_names(wallet_);

  if (!wallet_.empty())
  {
    boost::system::error_code ignored_ec;
    THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_wallet_file, ignored_ec), error::file_exists, m_wallet_file);
    THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_keys_file,   ignored_ec), error::file_exists, m_keys_file);
  }

  m_account.create_from_viewkey(account_public_address, viewkey);
  m_account_public_address = account_public_address;
  m_watch_only = true;
  m_multisig = false;
  m_multisig_threshold = 0;
  m_multisig_signers.clear();
  m_key_on_device = false;

  if (!wallet_.empty())
  {
    bool r = store_keys(m_keys_file, password, true);
    THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);

    if (m_nettype != MAINNET || create_address_file)
    {
      r = file_io_utils::save_string_to_file(m_wallet_file + ".address.txt", m_account.get_public_address_str(m_nettype));
      if(!r) MERROR("String with address text not saved");
    }
  }

  cryptonote::block b;
  generate_genesis(b);
  m_blockchain.push_back(get_block_hash(b));
  add_subaddress_account(tr("Primary account"));

  if (!wallet_.empty())
    store();
}

/*!
* \brief Creates a wallet from a public address and a spend/view secret key pair.
* \param  wallet_        Name of wallet file
* \param  password       Password of wallet file
* \param  spendkey       spend secret key
* \param  viewkey        view secret key
*/
void wallet::generate(const std::string& wallet_, const epee::wipeable_string& password,
  const cryptonote::account_public_address &account_public_address,
  const crypto::secret_key& spendkey, const crypto::secret_key& viewkey, bool create_address_file)
{
  clear();
  prepare_file_names(wallet_);

  if (!wallet_.empty())
  {
    boost::system::error_code ignored_ec;
    THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_wallet_file, ignored_ec), error::file_exists, m_wallet_file);
    THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_keys_file,   ignored_ec), error::file_exists, m_keys_file);
  }

  m_account.create_from_keys(account_public_address, spendkey, viewkey);
  m_account_public_address = account_public_address;
  m_watch_only = false;
  m_multisig = false;
  m_multisig_threshold = 0;
  m_multisig_signers.clear();
  m_key_on_device = false;

  if (!wallet_.empty())
  {
    bool r = store_keys(m_keys_file, password, false);
    THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);

    if (m_nettype != MAINNET || create_address_file)
    {
      r = file_io_utils::save_string_to_file(m_wallet_file + ".address.txt", m_account.get_public_address_str(m_nettype));
      if(!r) MERROR("String with address text not saved");
    }
  }

  cryptonote::block b;
  generate_genesis(b);
  m_blockchain.push_back(get_block_hash(b));
  add_subaddress_account(tr("Primary account"));

  if (!wallet_.empty())
    store();
}

/*!
* \brief Creates a wallet from a device
* \param  wallet_        Name of wallet file
* \param  password       Password of wallet file
* \param  device_name    device string address
*/
void wallet::restore(const std::string& wallet_, const epee::wipeable_string& password, const std::string &device_name)
{
  clear();
  prepare_file_names(wallet_);

  boost::system::error_code ignored_ec;
  if (!wallet_.empty()) {
    THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_wallet_file, ignored_ec), error::file_exists, m_wallet_file);
    THROW_WALLET_EXCEPTION_IF(boost::filesystem::exists(m_keys_file,   ignored_ec), error::file_exists, m_keys_file);
  }
  m_key_on_device = true;
  m_account.create_from_device(device_name);
  m_account_public_address = m_account.get_keys().m_account_address;
  m_watch_only = false;
  m_multisig = false;
  m_multisig_threshold = 0;
  m_multisig_signers.clear();

  if (!wallet_.empty()) {
    bool r = store_keys(m_keys_file, password, false);
    THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);

    r = file_io_utils::save_string_to_file(m_wallet_file + ".address.txt", m_account.get_public_address_str(m_nettype));
    if(!r) MERROR("String with address text not saved");
  }
  cryptonote::block b;
  generate_genesis(b);
  m_blockchain.push_back(get_block_hash(b));
  add_subaddress_account(tr("Primary account"));
  if (!wallet_.empty()) {
    store();
  }
}


bool wallet::multisig(bool *ready, uint32_t *threshold, uint32_t *total) const
{
  return false;
}


/*!
 * \brief Rewrites to the wallet file for wallet upgrade (doesn't generate key, assumes it's already there)
 * \param wallet_name Name of wallet file (should exist)
 * \param password    Password for wallet file
 */
void wallet::rewrite(const std::string& wallet_name, const epee::wipeable_string& password)
{
  if (wallet_name.empty())
    return;
  prepare_file_names(wallet_name);
  boost::system::error_code ignored_ec;
  THROW_WALLET_EXCEPTION_IF(!boost::filesystem::exists(m_keys_file, ignored_ec), error::file_not_found, m_keys_file);
  bool r = store_keys(m_keys_file, password, m_watch_only);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);
}
/*!
 * \brief Writes to a file named based on the normal wallet (doesn't generate key, assumes it's already there)
 * \param wallet_name Base name of wallet file
 * \param password    Password for wallet file
 */
void wallet::write_watch_only_wallet(const std::string& wallet_name, const epee::wipeable_string& password, std::string &new_keys_filename)
{
  prepare_file_names(wallet_name);
  boost::system::error_code ignored_ec;
  new_keys_filename = m_wallet_file + "-watchonly.keys";
  bool watch_only_keys_file_exists = boost::filesystem::exists(new_keys_filename, ignored_ec);
  THROW_WALLET_EXCEPTION_IF(watch_only_keys_file_exists, error::file_save_error, new_keys_filename);
  bool r = store_keys(new_keys_filename, password, true);
  THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, new_keys_filename);
}
//----------------------------------------------------------------------------------------------------
void wallet::wallet_exists(const std::string& file_path, bool& keys_file_exists, bool& wallet_file_exists)
{
  std::string keys_file, wallet_file;
  do_prepare_file_names(file_path, keys_file, wallet_file);

  boost::system::error_code ignore;
  keys_file_exists = boost::filesystem::exists(keys_file, ignore);
  wallet_file_exists = boost::filesystem::exists(wallet_file, ignore);
}
//----------------------------------------------------------------------------------------------------
bool wallet::wallet_valid_path_format(const std::string& file_path)
{
  return !file_path.empty();
}
//----------------------------------------------------------------------------------------------------
bool wallet::parse_long_payment_id(const std::string& payment_id_str, crypto::hash& payment_id)
{
  cryptonote::blobdata payment_id_data;
  if(!epee::string_tools::parse_hexstr_to_binbuff(payment_id_str, payment_id_data))
    return false;

  if(sizeof(crypto::hash) != payment_id_data.size())
    return false;

  payment_id = *reinterpret_cast<const crypto::hash*>(payment_id_data.data());
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::parse_short_payment_id(const std::string& payment_id_str, crypto::hash8& payment_id)
{
  cryptonote::blobdata payment_id_data;
  if(!epee::string_tools::parse_hexstr_to_binbuff(payment_id_str, payment_id_data))
    return false;

  if(sizeof(crypto::hash8) != payment_id_data.size())
    return false;

  payment_id = *reinterpret_cast<const crypto::hash8*>(payment_id_data.data());
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::parse_payment_id(const std::string& payment_id_str, crypto::hash& payment_id)
{
  if (parse_long_payment_id(payment_id_str, payment_id))
    return true;
  crypto::hash8 payment_id8;
  if (parse_short_payment_id(payment_id_str, payment_id8))
  {
    memcpy(payment_id.data, payment_id8.data, 8);
    memset(payment_id.data + 8, 0, 24);
    return true;
  }
  return false;
}
//----------------------------------------------------------------------------------------------------
bool wallet::prepare_file_names(const std::string& file_path)
{
  do_prepare_file_names(file_path, m_keys_file, m_wallet_file);
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::check_connection(uint32_t *version, uint32_t timeout)
{
  THROW_WALLET_EXCEPTION_IF(!m_is_initialized, error::wallet_not_initialized);

  boost::lock_guard<boost::mutex> lock(m_daemon_rpc_mutex);

  // TODO: Add light wallet version check.
  if(m_light_wallet) {
      version = 0;
      return m_light_wallet_connected;
  }

  if(!m_http_client.is_connected())
  {
    m_node_rpc_proxy.invalidate();
    if (!m_http_client.connect(std::chrono::milliseconds(timeout)))
      return false;
  }

  if (version)
  {
    cryptonote::COMMAND_RPC_GET_VERSION::request req_t = AUTO_VAL_INIT(req_t);
    cryptonote::COMMAND_RPC_GET_VERSION::response resp_t = AUTO_VAL_INIT(resp_t);
    bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_version", req_t, resp_t, m_http_client);
    if(!r) {
      *version = 0;
      return false;
    }
    if (resp_t.status != CORE_RPC_STATUS_OK)
      *version = 0;
    else
      *version = resp_t.version;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::generate_chacha_key_from_secret_keys(crypto::chacha_key &key) const
{
  hw::device &hwdev =  m_account.get_device();
  return hwdev.generate_chacha_key(m_account.get_keys(), key);
}
//----------------------------------------------------------------------------------------------------
void wallet::load(const std::string& wallet_, const epee::wipeable_string& password)
{
  clear();
  prepare_file_names(wallet_);

  boost::system::error_code e;
  bool exists = boost::filesystem::exists(m_keys_file, e);
  THROW_WALLET_EXCEPTION_IF(e || !exists, error::file_not_found, m_keys_file);

  if (!load_keys(m_keys_file, password))
  {
    THROW_WALLET_EXCEPTION_IF(true, error::file_read_error, m_keys_file);
  }
  LOG_PRINT_L0("Loaded wallet keys file, with public address: " << m_account.get_public_address_str(m_nettype));

  //keys loaded ok!
  //try to load wallet file. but even if we failed, it is not big problem
  if(!boost::filesystem::exists(m_wallet_file, e) || e)
  {
    LOG_PRINT_L0("file not found: " << m_wallet_file << ", starting with empty blockchain");
    m_account_public_address = m_account.get_keys().m_account_address;
  }
  else
  {
    wallet::cache_file_data cache_file_data;
    std::string buf;
    bool r = epee::file_io_utils::load_file_to_string(m_wallet_file, buf);
    THROW_WALLET_EXCEPTION_IF(!r, error::file_read_error, m_wallet_file);

    // try to read it as an encrypted cache
    try
    {
      LOG_PRINT_L1("Trying to decrypt cache data");

      r = ::serialization::parse_binary(buf, cache_file_data);
      THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "internal error: failed to deserialize \"" + m_wallet_file + '\"');
      crypto::chacha_key key;
      generate_chacha_key_from_secret_keys(key);
      std::string cache_data;
      cache_data.resize(cache_file_data.cache_data.size());
      crypto::chacha20(cache_file_data.cache_data.data(), cache_file_data.cache_data.size(), key, cache_file_data.iv, &cache_data[0]);

      try {
        std::stringstream iss;
        iss << cache_data;
        boost::archive::portable_binary_iarchive ar(iss);
        ar >> *this;
      }
      catch (...)
      {
        crypto::chacha8(cache_file_data.cache_data.data(), cache_file_data.cache_data.size(), key, cache_file_data.iv, &cache_data[0]);
        try
        {
          std::stringstream iss;
          iss << cache_data;
          boost::archive::portable_binary_iarchive ar(iss);
          ar >> *this;
        }
        catch (...)
        {
          LOG_PRINT_L0("Failed to open portable binary, trying unportable");
          boost::filesystem::copy_file(m_wallet_file, m_wallet_file + ".unportable", boost::filesystem::copy_option::overwrite_if_exists);
          std::stringstream iss;
          iss.str("");
          iss << cache_data;
          boost::archive::binary_iarchive ar(iss);
          ar >> *this;
        }
      }
    }
    catch (...)
    {
      LOG_PRINT_L1("Failed to load encrypted cache, trying unencrypted");
      try {
        std::stringstream iss;
        iss << buf;
        boost::archive::portable_binary_iarchive ar(iss);
        ar >> *this;
      }
      catch (...)
      {
        LOG_PRINT_L0("Failed to open portable binary, trying unportable");
        boost::filesystem::copy_file(m_wallet_file, m_wallet_file + ".unportable", boost::filesystem::copy_option::overwrite_if_exists);
        std::stringstream iss;
        iss.str("");
        iss << buf;
        boost::archive::binary_iarchive ar(iss);
        ar >> *this;
      }
    }
    THROW_WALLET_EXCEPTION_IF(
      m_account_public_address.m_spend_public_key != m_account.get_keys().m_account_address.m_spend_public_key ||
      m_account_public_address.m_view_public_key  != m_account.get_keys().m_account_address.m_view_public_key,
      error::wallet_files_doesnt_correspond, m_keys_file, m_wallet_file);
  }

  cryptonote::block genesis;
  generate_genesis(genesis);
  crypto::hash genesis_hash = get_block_hash(genesis);

  if (m_blockchain.empty())
  {
    m_blockchain.push_back(genesis_hash);
  }
  else
  {
    check_genesis(genesis_hash);
  }

  trim_hashchain();

  if (get_num_subaddress_accounts() == 0)
    add_subaddress_account(tr("Primary account"));

  m_local_bc_height = m_blockchain.size();

  try
  {
    find_and_save_rings(false);
  }
  catch (const std::exception &e)
  {
    MERROR("Failed to save rings, will try again next time");
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::trim_hashchain()
{
  uint64_t height = m_checkpoints.get_max_height();

  for (const transfer_details &td: m_transfers)
    if (td.m_block_height < height)
      height = td.m_block_height;

  if (!m_blockchain.empty() && m_blockchain.size() == m_blockchain.offset())
  {
    MINFO("Fixing empty hashchain");
    cryptonote::COMMAND_RPC_GET_BLOCK_HEADER_BY_HEIGHT::request req = AUTO_VAL_INIT(req);
    cryptonote::COMMAND_RPC_GET_BLOCK_HEADER_BY_HEIGHT::response res = AUTO_VAL_INIT(res);
    m_daemon_rpc_mutex.lock();
    req.height = m_blockchain.size() - 1;
    bool r = net_utils::invoke_http_json_rpc("/json_rpc", "getblockheaderbyheight", req, res, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    if (r && res.status == CORE_RPC_STATUS_OK)
    {
      crypto::hash hash;
      epee::string_tools::hex_to_pod(res.block_header.hash, hash);
      m_blockchain.refill(hash);
    }
    else
    {
      MERROR("Failed to request block header from daemon, hash chain may be unable to sync till the wallet is loaded with a usable daemon");
    }
  }
  if (height > 0 && m_blockchain.size() > height)
  {
    --height;
    MDEBUG("trimming to " << height << ", offset " << m_blockchain.offset());
    m_blockchain.trim(height);
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::check_genesis(const crypto::hash& genesis_hash) const {
  std::string what("Genesis block mismatch. You probably use wallet without testnet (or stagenet) flag with blockchain from test (or stage) network or vice versa");

  THROW_WALLET_EXCEPTION_IF(genesis_hash != m_blockchain.genesis(), error::wallet_internal_error, what);
}
//----------------------------------------------------------------------------------------------------
std::string wallet::path() const
{
  return m_wallet_file;
}
//----------------------------------------------------------------------------------------------------
void wallet::store()
{
  store_to("", epee::wipeable_string());
}
//----------------------------------------------------------------------------------------------------
void wallet::store_to(const std::string &path, const epee::wipeable_string &password)
{
  trim_hashchain();

  // if file is the same, we do:
  // 1. save wallet to the *.new file
  // 2. remove old wallet file
  // 3. rename *.new to wallet_name

  // handle if we want just store wallet state to current files (ex store() replacement);
  bool same_file = true;

#ifdef WIN32
//boost canonical messes with linux gnu
#else
  if (!path.empty())
  {
    std::string canonical_path = boost::filesystem::canonical(m_wallet_file).string();
    size_t pos = canonical_path.find(path);
    same_file = pos != std::string::npos;
  }
#endif

  if (!same_file)
  {
    // check if we want to store to directory which doesn't exists yet
    boost::filesystem::path parent_path = boost::filesystem::path(path).parent_path();

    // if path is not exists, try to create it
    if (!parent_path.empty() &&  !boost::filesystem::exists(parent_path))
    {
      boost::system::error_code ec;
      if (!boost::filesystem::create_directories(parent_path, ec))
      {
        throw std::logic_error(ec.message());
      }
    }
  }
  // preparing wallet data
  std::stringstream oss;
  boost::archive::portable_binary_oarchive ar(oss);
  ar << *this;

  wallet::cache_file_data cache_file_data = boost::value_initialized<wallet::cache_file_data>();
  cache_file_data.cache_data = oss.str();
  crypto::chacha_key key;
  generate_chacha_key_from_secret_keys(key);
  std::string cipher;
  cipher.resize(cache_file_data.cache_data.size());
  cache_file_data.iv = crypto::rand<crypto::chacha_iv>();
  crypto::chacha20(cache_file_data.cache_data.data(), cache_file_data.cache_data.size(), key, cache_file_data.iv, &cipher[0]);
  cache_file_data.cache_data = cipher;

  const std::string new_file = same_file ? m_wallet_file + ".new" : path;
  const std::string old_file = m_wallet_file;
  const std::string old_keys_file = m_keys_file;
  const std::string old_address_file = m_wallet_file + ".address.txt";

  // save keys to the new file
  // if we here, main wallet file is saved and we only need to save keys and address files
  if (!same_file) {
    prepare_file_names(path);
    bool r = store_keys(m_keys_file, password, false);
    THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_keys_file);
    if (boost::filesystem::exists(old_address_file))
    {
      // save address to the new file
      const std::string address_file = m_wallet_file + ".address.txt";
      r = file_io_utils::save_string_to_file(address_file, m_account.get_public_address_str(m_nettype));
      THROW_WALLET_EXCEPTION_IF(!r, error::file_save_error, m_wallet_file);
    }
    // remove old wallet file
    r = boost::filesystem::remove(old_file);
    if (!r) {
      LOG_ERROR("error removing file: " << old_file);
    }
    // remove old keys file
    r = boost::filesystem::remove(old_keys_file);
    if (!r) {
      LOG_ERROR("error removing file: " << old_keys_file);
    }
    // remove old address file
    r = boost::filesystem::remove(old_address_file);
    if (!r) {
      LOG_ERROR("error removing file: " << old_address_file);
    }
  } else {
    // save to new file
#ifdef WIN32
    // On Windows avoid using std::ofstream which does not work with UTF-8 filenames
    // The price to pay is temporary higher memory consumption for string stream + binary archive
    std::ostringstream oss;
    binary_archive<true> oar(oss);
    bool success = ::serialization::serialize(oar, cache_file_data);
    if (success) {
        success = epee::file_io_utils::save_string_to_file(new_file, oss.str());
    }
    THROW_WALLET_EXCEPTION_IF(!success, error::file_save_error, new_file);
#else
    std::ofstream ostr;
    ostr.open(new_file, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
    binary_archive<true> oar(ostr);
    bool success = ::serialization::serialize(oar, cache_file_data);
    ostr.close();
    THROW_WALLET_EXCEPTION_IF(!success || !ostr.good(), error::file_save_error, new_file);
#endif

    // here we have "*.new" file, we need to rename it to be without ".new"
    std::error_code e = tools::replace_file(new_file, m_wallet_file);
    THROW_WALLET_EXCEPTION_IF(e, error::file_save_error, m_wallet_file, e);
  }
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::balance(uint32_t index_major) const
{
  uint64_t amount = 0;
  if(m_light_wallet)
    return m_light_wallet_unlocked_balance;
  for (const auto& i : balance_per_subaddress(index_major))
    amount += i.second;
  return amount;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::unlocked_balance(uint32_t index_major) const
{
  uint64_t amount = 0;
  if(m_light_wallet)
    return m_light_wallet_balance;
  for (const auto& i : unlocked_balance_per_subaddress(index_major))
    amount += i.second;
  return amount;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::token_balance(uint32_t index_major) const
{
  uint64_t token_amount = 0;
  if(m_light_wallet)
    return m_light_wallet_token_balance;
  for (const auto& i : token_balance_per_subaddress(index_major))
    token_amount += i.second;
  return token_amount;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::unlocked_token_balance(uint32_t index_major) const
{
  uint64_t token_amount = 0;
  if(m_light_wallet)
    return m_light_wallet_unlocked_token_balance;
  for (const auto& i : unlocked_token_balance_per_subaddress(index_major))
    token_amount += i.second;
  return token_amount;
}
//----------------------------------------------------------------------------------------------------
std::map<uint32_t, uint64_t> wallet::balance_per_subaddress(uint32_t index_major) const
{
  std::map<uint32_t, uint64_t> amount_per_subaddr;
  for (const auto& td: m_transfers)
  {
    if(td.m_output_type != cryptonote::tx_out_type::out_cash) {
      continue;
    }

    if (td.m_subaddr_index.major == index_major && !td.m_spent)
    {
      auto found = amount_per_subaddr.find(td.m_subaddr_index.minor);
      if (found == amount_per_subaddr.end())
        amount_per_subaddr[td.m_subaddr_index.minor] = td.amount();
      else
        found->second += td.amount();
    }
  }
  for (const auto& utx: m_unconfirmed_txs)
  {
    if(  utx.second.m_output_type != cryptonote::tx_out_type::out_cash) {
      continue;
    }

    if (utx.second.m_subaddr_account == index_major && utx.second.m_state != wallet::unconfirmed_transfer_details::failed)
    {
      // all changes go to 0-th subaddress (in the current subaddress account)
      auto found = amount_per_subaddr.find(0);
      if (found == amount_per_subaddr.end())
        amount_per_subaddr[0] = utx.second.m_change;
      else
        found->second += utx.second.m_change;
    }
  }
  return amount_per_subaddr;
}
//----------------------------------------------------------------------------------------------------
std::map<uint32_t, uint64_t> wallet::unlocked_balance_per_subaddress(uint32_t index_major) const
{
  std::map<uint32_t, uint64_t> amount_per_subaddr;
  for(const transfer_details& td: m_transfers)
  {
    if(td.m_output_type != cryptonote::tx_out_type::out_cash) {
      continue;
    }

    if(td.m_subaddr_index.major == index_major && !td.m_spent && is_transfer_unlocked(td))
    {
      auto found = amount_per_subaddr.find(td.m_subaddr_index.minor);
      if (found == amount_per_subaddr.end())
        amount_per_subaddr[td.m_subaddr_index.minor] = td.amount();
      else
        found->second += td.amount();
    }
  }
  return amount_per_subaddr;
}

//----------------------------------------------------------------------------------------------------
std::map<uint32_t, uint64_t> wallet::token_balance_per_subaddress(uint32_t index_major) const
{
  std::map<uint32_t, uint64_t> token_amount_per_subaddr;
  for (const auto& td: m_transfers)
  {
    if(td.m_output_type != cryptonote::tx_out_type::out_token) {
      continue;
    }

    if (td.m_subaddr_index.major == index_major && !td.m_spent)
    {
      auto found = token_amount_per_subaddr.find(td.m_subaddr_index.minor);
      if (found == token_amount_per_subaddr.end())
        token_amount_per_subaddr[td.m_subaddr_index.minor] = td.get_out_type() == tx_out_type::out_token ? td.token_amount():0;
      else
        found->second += td.get_out_type() == tx_out_type::out_token ? td.token_amount() : 0;
    }
  }
  for (const auto& utx: m_unconfirmed_txs)
  {
    if(  utx.second.m_output_type != cryptonote::tx_out_type::out_token) {
      continue;
    }
    if (utx.second.m_subaddr_account == index_major && utx.second.m_state != wallet::unconfirmed_transfer_details::failed)
    {
      // all changes go to 0-th subaddress (in the current subaddress account)
      auto found = token_amount_per_subaddr.find(0);
      if (found == token_amount_per_subaddr.end())
        token_amount_per_subaddr[0] = utx.second.m_token_change;
      else
        found->second += utx.second.m_token_change;
    }
  }
  return token_amount_per_subaddr;
}

//----------------------------------------------------------------------------------------------------

//----------------------------------------------------------------------------------------------------
std::map<uint32_t, uint64_t> wallet::unlocked_token_balance_per_subaddress(uint32_t index_major) const
{
  std::map<uint32_t, uint64_t> token_amount_per_subaddr;
  for(const transfer_details& td: m_transfers)
  {
    if(td.m_output_type != cryptonote::tx_out_type::out_token) {
      continue;
    }

    if(td.m_subaddr_index.major == index_major && !td.m_spent && is_token_transfer_unlocked(td))
    {
      auto found = token_amount_per_subaddr.find(td.m_subaddr_index.minor);
      if (found == token_amount_per_subaddr.end())
        token_amount_per_subaddr[td.m_subaddr_index.minor] = td.m_output_type == tx_out_type::out_token ? td.token_amount() : 0;
      else
        found->second += td.m_output_type == tx_out_type::out_token ? td.token_amount() : 0;
    }
  }
  return token_amount_per_subaddr;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::balance_all() const
{
  uint64_t r = 0;
  for (uint32_t index_major = 0; index_major < get_num_subaddress_accounts(); ++index_major)
    r += balance(index_major);
  return r;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::unlocked_balance_all() const
{
  uint64_t r = 0;
  for (uint32_t index_major = 0; index_major < get_num_subaddress_accounts(); ++index_major)
    r += unlocked_balance(index_major);
  return r;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::token_balance_all() const
{
  uint64_t r = 0;
  for (uint32_t index_major = 0; index_major < get_num_subaddress_accounts(); ++index_major)
    r += token_balance(index_major);
  return r;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::unlocked_token_balance_all() const
{
  uint64_t r = 0;
  for (uint32_t index_major = 0; index_major < get_num_subaddress_accounts(); ++index_major)
    r += unlocked_token_balance(index_major);
  return r;
}
//----------------------------------------------------------------------------------------------------
void wallet::get_transfers(wallet::transfer_container& incoming_transfers) const
{
  incoming_transfers = m_transfers;
}
//----------------------------------------------------------------------------------------------------
void wallet::get_payments(const crypto::hash& payment_id, std::list<wallet::payment_details>& payments, uint64_t min_height, const boost::optional<uint32_t>& subaddr_account, const std::set<uint32_t>& subaddr_indices) const
{
  auto range = m_payments.equal_range(payment_id);
  std::for_each(range.first, range.second, [&payments, &min_height, &subaddr_account, &subaddr_indices](const payment_container::value_type& x) {
    if (min_height < x.second.m_block_height &&
      (!subaddr_account || *subaddr_account == x.second.m_subaddr_index.major) &&
      (subaddr_indices.empty() || subaddr_indices.count(x.second.m_subaddr_index.minor) == 1))
    {
      payments.push_back(x.second);
    }
  });
}
//----------------------------------------------------------------------------------------------------
void wallet::get_payments(std::list<std::pair<crypto::hash,wallet::payment_details>>& payments, uint64_t min_height, uint64_t max_height, const boost::optional<uint32_t>& subaddr_account, const std::set<uint32_t>& subaddr_indices) const
{
  auto range = std::make_pair(m_payments.begin(), m_payments.end());
  std::for_each(range.first, range.second, [&payments, &min_height, &max_height, &subaddr_account, &subaddr_indices](const payment_container::value_type& x) {
    if (min_height < x.second.m_block_height && max_height >= x.second.m_block_height &&
      (!subaddr_account || *subaddr_account == x.second.m_subaddr_index.major) &&
      (subaddr_indices.empty() || subaddr_indices.count(x.second.m_subaddr_index.minor) == 1))
    {
      payments.push_back(x);
    }
  });
}
//----------------------------------------------------------------------------------------------------
void wallet::get_payments_out(std::list<std::pair<crypto::hash,wallet::confirmed_transfer_details>>& confirmed_payments,
    uint64_t min_height, uint64_t max_height, const boost::optional<uint32_t>& subaddr_account, const std::set<uint32_t>& subaddr_indices) const
{
  for (auto i = m_confirmed_txs.begin(); i != m_confirmed_txs.end(); ++i) {
    if (i->second.m_block_height <= min_height || i->second.m_block_height > max_height)
      continue;
    if (subaddr_account && *subaddr_account != i->second.m_subaddr_account)
      continue;
    if (!subaddr_indices.empty() && std::count_if(i->second.m_subaddr_indices.begin(), i->second.m_subaddr_indices.end(), [&subaddr_indices](uint32_t index) { return subaddr_indices.count(index) == 1; }) == 0)
      continue;
    confirmed_payments.push_back(*i);
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::get_unconfirmed_payments_out(std::list<std::pair<crypto::hash,wallet::unconfirmed_transfer_details>>& unconfirmed_payments, const boost::optional<uint32_t>& subaddr_account, const std::set<uint32_t>& subaddr_indices) const
{
  for (auto i = m_unconfirmed_txs.begin(); i != m_unconfirmed_txs.end(); ++i) {
    if (subaddr_account && *subaddr_account != i->second.m_subaddr_account)
      continue;
    if (!subaddr_indices.empty() && std::count_if(i->second.m_subaddr_indices.begin(), i->second.m_subaddr_indices.end(), [&subaddr_indices](uint32_t index) { return subaddr_indices.count(index) == 1; }) == 0)
      continue;
    unconfirmed_payments.push_back(*i);
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::get_unconfirmed_payments(std::list<std::pair<crypto::hash,wallet::pool_payment_details>>& unconfirmed_payments, const boost::optional<uint32_t>& subaddr_account, const std::set<uint32_t>& subaddr_indices) const
{
  for (auto i = m_unconfirmed_payments.begin(); i != m_unconfirmed_payments.end(); ++i) {
    if ((!subaddr_account || *subaddr_account == i->second.m_pd.m_subaddr_index.major) &&
      (subaddr_indices.empty() || subaddr_indices.count(i->second.m_pd.m_subaddr_index.minor) == 1))
    unconfirmed_payments.push_back(*i);
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::rescan_spent()
{
  // This is RPC call that can take a long time if there are many outputs,
  // so we call it several times, in stripes, so we don't time out spuriously
  std::vector<int> spent_status;
  spent_status.reserve(m_transfers.size());
  const size_t chunk_size = 1000;
  for (size_t start_offset = 0; start_offset < m_transfers.size(); start_offset += chunk_size)
  {
    const size_t n_outputs = std::min<size_t>(chunk_size, m_transfers.size() - start_offset);
    MDEBUG("Calling is_key_image_spent on " << start_offset << " - " << (start_offset + n_outputs - 1) << ", out of " << m_transfers.size());
    COMMAND_RPC_IS_KEY_IMAGE_SPENT::request req = AUTO_VAL_INIT(req);
    COMMAND_RPC_IS_KEY_IMAGE_SPENT::response daemon_resp = AUTO_VAL_INIT(daemon_resp);
    for (size_t n = start_offset; n < start_offset + n_outputs; ++n)
      req.key_images.push_back(string_tools::pod_to_hex(m_transfers[n].m_key_image));
    m_daemon_rpc_mutex.lock();
    bool r = epee::net_utils::invoke_http_json("/is_key_image_spent", req, daemon_resp, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "is_key_image_spent");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "is_key_image_spent");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status != CORE_RPC_STATUS_OK, error::is_key_image_spent_error, daemon_resp.status);
    THROW_WALLET_EXCEPTION_IF(daemon_resp.spent_status.size() != n_outputs, error::wallet_internal_error,
      "daemon returned wrong response for is_key_image_spent, wrong amounts count = " +
      std::to_string(daemon_resp.spent_status.size()) + ", expected " +  std::to_string(n_outputs));
    std::copy(daemon_resp.spent_status.begin(), daemon_resp.spent_status.end(), std::back_inserter(spent_status));
  }

  // update spent status
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    transfer_details& td = m_transfers[i];
    // a view wallet may not know about key images
    if (!td.m_key_image_known || td.m_key_image_partial)
      continue;
    if (td.m_spent != (spent_status[i] != COMMAND_RPC_IS_KEY_IMAGE_SPENT::UNSPENT))
    {
      if (td.m_spent)
      {
        LOG_PRINT_L0("Marking output " << i << "(" << td.m_key_image << ") as unspent, it was marked as spent");
        set_unspent(i);
        td.m_spent_height = 0;
      }
      else
      {
        LOG_PRINT_L0("Marking output " << i << "(" << td.m_key_image << ") as spent, it was marked as unspent");
        set_spent(i, td.m_spent_height);
        // unknown height, if this gets reorged, it might still be missed
      }
    }
  }
}
//----------------------------------------------------------------------------------------------------
void wallet::rescan_blockchain(bool refresh)
{
  clear();

  cryptonote::block genesis;
  generate_genesis(genesis);
  crypto::hash genesis_hash = get_block_hash(genesis);
  m_blockchain.push_back(genesis_hash);
  add_subaddress_account(tr("Primary account"));
  m_local_bc_height = 1;

  if (refresh)
    this->refresh();
}
//----------------------------------------------------------------------------------------------------
bool wallet::is_transfer_unlocked(const transfer_details& td) const
{
  return is_transfer_unlocked(td.m_tx.unlock_time, td.m_block_height);
}
//----------------------------------------------------------------------------------------------------
bool wallet::is_transfer_unlocked(uint64_t unlock_time, uint64_t block_height) const
{
  if(!is_tx_spendtime_unlocked(unlock_time, block_height))
    return false;

  if(block_height + CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE > m_local_bc_height)
    return false;

  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::is_token_transfer_unlocked(const transfer_details& td) const
{
  return is_token_transfer_unlocked(td.m_tx.unlock_time, td.m_block_height);
}
//----------------------------------------------------------------------------------------------------
bool wallet::is_token_transfer_unlocked(uint64_t unlock_time, uint64_t block_height) const
{
  if(!is_tx_spendtime_unlocked(unlock_time, block_height))
    return false;

  if(block_height + CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE > m_local_bc_height)
    return false;

  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::is_tx_spendtime_unlocked(uint64_t unlock_time, uint64_t block_height) const
{
  if(unlock_time < CRYPTONOTE_MAX_BLOCK_NUMBER)
  {
    //interpret as block index
    if(m_local_bc_height-1 + CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_BLOCKS >= unlock_time)
      return true;
    else
      return false;
  }else
  {
    //interpret as time
    uint64_t current_time = static_cast<uint64_t>(time(NULL));
    // XXX: this needs to be fast, so we'd need to get the starting heights
    // from the daemon to be correct once voting kicks in
    uint64_t v2height = m_nettype == TESTNET ? 624634 : m_nettype == STAGENET ? (uint64_t)-1/*TODO*/ : 1009827;
    uint64_t leeway = CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_SECONDS;
    if(current_time + leeway >= unlock_time)
      return true;
    else
      return false;
  }
  return false;
}
//----------------------------------------------------------------------------------------------------
namespace
{
  template<typename T>
  T pop_index(std::vector<T>& vec, size_t idx)
  {
    CHECK_AND_ASSERT_MES(!vec.empty(), T(), "Vector must be non-empty");
    CHECK_AND_ASSERT_MES(idx < vec.size(), T(), "idx out of bounds");

    T res = vec[idx];
    if (idx + 1 != vec.size())
    {
      vec[idx] = vec.back();
    }
    vec.resize(vec.size() - 1);

    return res;
  }

  template<typename T>
  T pop_random_value(std::vector<T>& vec)
  {
    CHECK_AND_ASSERT_MES(!vec.empty(), T(), "Vector must be non-empty");

    size_t idx = crypto::rand<size_t>() % vec.size();
    return pop_index (vec, idx);
  }

  template<typename T>
  T pop_back(std::vector<T>& vec)
  {
    CHECK_AND_ASSERT_MES(!vec.empty(), T(), "Vector must be non-empty");

    T res = vec.back();
    vec.pop_back();
    return res;
  }

  template<typename T>
  void pop_if_present(std::vector<T>& vec, T e)
  {
    for (size_t i = 0; i < vec.size(); ++i)
    {
      if (e == vec[i])
      {
        pop_index (vec, i);
        return;
      }
    }
  }
}
//----------------------------------------------------------------------------------------------------
// This returns a handwavy estimation of how much two outputs are related
// If they're from the same tx, then they're fully related. From close block
// heights, they're kinda related. The actual values don't matter, just
// their ordering, but it could become more murky if we add scores later.
float wallet::get_output_relatedness(const transfer_details &td0, const transfer_details &td1) const
{
  int dh;

  // expensive test, and same tx will fall onto the same block height below
  if (td0.m_txid == td1.m_txid)
    return 1.0f;

  // same block height -> possibly tx burst, or same tx (since above is disabled)
  dh = td0.m_block_height > td1.m_block_height ? td0.m_block_height - td1.m_block_height : td1.m_block_height - td0.m_block_height;
  if (dh == 0)
    return 0.9f;

  // adjacent blocks -> possibly tx burst
  if (dh == 1)
    return 0.8f;

  // could extract the payment id, and compare them, but this is a bit expensive too

  // similar block heights
  if (dh < 10)
    return 0.2f;

  // don't think these are particularly related
  return 0.0f;
}
//----------------------------------------------------------------------------------------------------
size_t wallet::pop_best_value_from(const transfer_container &transfers, std::vector<size_t> &unused_indices, const std::vector<size_t>& selected_transfers, bool smallest, const cryptonote::tx_out_type out_type) const
{
  std::vector<size_t> candidates;
  float best_relatedness = 1.0f;
  for (size_t n = 0; n < unused_indices.size(); ++n)
  {
    const transfer_details &candidate = transfers[unused_indices[n]];
    if (candidate.get_out_type() != out_type) continue;
    float relatedness = 0.0f;
    for (std::vector<size_t>::const_iterator i = selected_transfers.begin(); i != selected_transfers.end(); ++i)
    {
      float r = get_output_relatedness(candidate, transfers[*i]);
      if (r > relatedness)
      {
        relatedness = r;
        if (relatedness == 1.0f)
          break;
      }
    }

    if (relatedness < best_relatedness)
    {
      best_relatedness = relatedness;
      candidates.clear();
    }

    if (relatedness == best_relatedness)
      candidates.push_back(n);
  }

  // we have all the least related outputs in candidates, so we can pick either
  // the smallest, or a random one, depending on request
  size_t idx;
  if (smallest)
  {
    idx = 0;
    for (size_t n = 0; n < candidates.size(); ++n)
    {
      const transfer_details &td = transfers[unused_indices[candidates[n]]];
      if (out_type == tx_out_type::out_token && td.get_out_type() == out_type)
      {
        if (td.token_amount() < transfers[unused_indices[candidates[idx]]].token_amount())
          idx = n;
      }
      else if (out_type == tx_out_type::out_cash && td.get_out_type() == out_type)
      {
        if (td.amount() < transfers[unused_indices[candidates[idx]]].amount())
          idx = n;
      }
      else if (out_type == tx_out_type::out_staked_token && td.get_out_type() == out_type)
      {
        if (td.token_amount() == transfers[unused_indices[candidates[idx]]].token_amount())
          idx = n;
      }
    }
  }
  else
  {
    idx = crypto::rand<size_t>() % candidates.size();
  }
  return pop_index (unused_indices, candidates[idx]);
}
//----------------------------------------------------------------------------------------------------
  size_t wallet::pop_ideal_value_from(const transfer_container &transfers, std::vector<size_t> &unused_indices, const std::vector<size_t> &selected_transfers, const cryptonote::tx_out_type out_type, const uint64_t cash_amount,
                                      const uint64_t token_amount) const
  {
    std::vector<size_t> candidates;
    float best_relatedness = 1.0f;
    uint64_t oldest_output = get_blockchain_current_height();
    for (size_t n = 0; n < unused_indices.size(); ++n)
    {
      const transfer_details &candidate = transfers[unused_indices[n]];
      if ((candidate.get_out_type() != out_type) || (candidate.token_amount() != token_amount) || (candidate.amount() != cash_amount)) continue;

      //in case of staked token outputs with the same amount, select the oldest one
      if (candidate.get_out_type() == tx_out_type::out_staked_token)
      {
        if (candidate.m_block_height < oldest_output)
        {
          candidates.clear();
          oldest_output = candidate.m_block_height;
          candidates.push_back(n);
        }
      }
      else
      {
        float relatedness = 0.0f;
        for (std::vector<size_t>::const_iterator i = selected_transfers.begin(); i != selected_transfers.end(); ++i)
        {
          float r = get_output_relatedness(candidate, transfers[*i]);
          if (r > relatedness)
          {
            relatedness = r;
            if (relatedness == 1.0f)
              break;
          }
        }


        if (relatedness < best_relatedness)
        {
          best_relatedness = relatedness;
          candidates.clear();
        }

        if (relatedness == best_relatedness)
          candidates.push_back(n);
      }
    }


    THROW_WALLET_EXCEPTION_IF(candidates.empty(), error::no_matching_available_outputs);

    size_t idx;
    idx = crypto::rand<size_t>() % candidates.size();

    return pop_index(unused_indices, candidates[idx]);
  }
  //----------------------------------------------------------------------------------------------------
  size_t wallet::pop_advanced_output_from(const transfer_container &transfers,const std::vector<size_t>& selected_transfers, const std::string &acc_username, const cryptonote::tx_out_type out_type) const
  {
    std::vector<size_t> candidates;
    for (size_t n = 0; n < transfers.size(); ++n)
    {
      const transfer_details &candidate = transfers[n];
      if (candidate.get_out_type() != out_type) continue;
      candidates.push_back(n);
    }


    THROW_WALLET_EXCEPTION_IF(candidates.empty(), error::safex_unknown_account);

    int idx = -1;
    for (size_t n = 0; n < candidates.size(); ++n)
    {
      const transfer_details &td = transfers[candidates[n]];
      if (out_type == tx_out_type::out_safex_account && td.get_out_type() == tx_out_type::out_safex_account)
      {
          const txout_to_script &current = boost::get<const cryptonote::txout_to_script&>(td.m_tx.vout[td.m_internal_output_index].target);
          const cryptonote::blobdata blobdata1(begin(current.data), end(current.data));
          safex::create_account_data account_output_data;
          parse_and_validate_object_from_blob(blobdata1, account_output_data);
          const std::string current_username = std::string(begin(account_output_data.username), end(account_output_data.username));
          if (current_username == acc_username) idx = (int)n;
      }
    }

    THROW_WALLET_EXCEPTION_IF(idx == -1, error::safex_unknown_account);

    return candidates[idx];
  }
//----------------------------------------------------------------------------------------------------
size_t wallet::pop_best_value(std::vector<size_t> &unused_indices, const std::vector<size_t>& selected_transfers, bool smallest, const cryptonote::tx_out_type out_type) const
{
  return pop_best_value_from(m_transfers, unused_indices, selected_transfers, smallest, out_type);
}
//----------------------------------------------------------------------------------------------------
size_t wallet::pop_ideal_value(std::vector<size_t> &unused_indices, const std::vector<size_t>& selected_transfers, const cryptonote::tx_out_type out_type, const uint64_t cash_amount, const uint64_t token_amount) const
{
  return pop_ideal_value_from(m_transfers, unused_indices, selected_transfers, out_type, cash_amount, token_amount);
}
//----------------------------------------------------------------------------------------------------
  size_t wallet::pop_advanced_output(const std::vector<size_t>& selected_transfers, const std::vector<uint8_t> &acc_username, const cryptonote::tx_out_type out_type) const
  {
    const std::string acc_username_str(acc_username.begin(), acc_username.end());
    return pop_advanced_output_from(m_transfers, selected_transfers,  acc_username_str, out_type);
  }
//----------------------------------------------------------------------------------------------------
// Select random input sources for transaction.
// returns:
//    direct return: amount of money found
//    modified reference: selected_transfers, a list of iterators/indices of input sources
uint64_t wallet::select_transfers(uint64_t needed_money, std::vector<size_t> unused_transfers_indices, std::vector<size_t>& selected_transfers, bool trusted_daemon) const
{
  uint64_t found_money = 0;
  selected_transfers.reserve(unused_transfers_indices.size());
  while (found_money < needed_money && !unused_transfers_indices.empty())
  {
    size_t idx = pop_best_value(unused_transfers_indices, selected_transfers);

    const transfer_container::const_iterator it = m_transfers.begin() + idx;
    selected_transfers.push_back(idx);
    found_money += it->amount();
  }

  return found_money;
}
//----------------------------------------------------------------------------------------------------
  void wallet::add_unconfirmed_tx(const cryptonote::transaction &tx, uint64_t amount_in, uint64_t token_amount_in, const std::vector<cryptonote::tx_destination_entry> &dests, const crypto::hash &payment_id, uint64_t change_amount,
                                    uint64_t token_change_amount, uint32_t subaddr_account, const std::set<uint32_t> &subaddr_indices)
{
  unconfirmed_transfer_details& utd = m_unconfirmed_txs[cryptonote::get_transaction_hash(tx)];
  utd.m_amount_in = amount_in;
  utd.m_token_amount_in = token_amount_in;
  utd.m_amount_out = 0;
  utd.m_token_amount_out = 0;
  for (const auto &d: dests)
  {
    utd.m_amount_out += d.amount;
    utd.m_token_amount_out += d.token_amount;
  }

  utd.m_amount_out += change_amount; // dests does not contain change
  utd.m_token_amount_out += token_change_amount; // dests does not contain change
  utd.m_change = change_amount;
  utd.m_token_change = token_change_amount;
  utd.m_sent_time = time(NULL);
  utd.m_tx = (const cryptonote::transaction_prefix&)tx;
  utd.m_dests = dests;
  utd.m_payment_id = payment_id;
  utd.m_state = wallet::unconfirmed_transfer_details::pending;
  utd.m_timestamp = time(NULL);
  utd.m_subaddr_account = subaddr_account;
  utd.m_subaddr_indices = subaddr_indices;
  for (const auto &in: tx.vin)
  {
    if (in.type() != typeid(cryptonote::txin_to_key))
      continue;
    const auto &txin = boost::get<cryptonote::txin_to_key>(in);
    utd.m_rings.push_back(std::make_pair(txin.k_image, txin.key_offsets));
  }
}

//----------------------------------------------------------------------------------------------------
void wallet::transfer(const std::vector<cryptonote::tx_destination_entry>& dsts, const size_t fake_outs_count, const std::vector<size_t> &unused_transfers_indices,
                       uint64_t unlock_time, uint64_t fee, const std::vector<uint8_t>& extra, cryptonote::transaction& tx, pending_tx& ptx, bool trusted_daemon)
{
  transfer(dsts, fake_outs_count, unused_transfers_indices, unlock_time, fee, extra, detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), tx, ptx, trusted_daemon);
}
//----------------------------------------------------------------------------------------------------
void wallet::transfer(const std::vector<cryptonote::tx_destination_entry>& dsts, const size_t fake_outs_count, const std::vector<size_t> &unused_transfers_indices,
                       uint64_t unlock_time, uint64_t fee, const std::vector<uint8_t>& extra, bool trusted_daemon)
{
  cryptonote::transaction tx;
  pending_tx ptx = AUTO_VAL_INIT(ptx);
  transfer(dsts, fake_outs_count, unused_transfers_indices, unlock_time, fee, extra, tx, ptx, trusted_daemon);
}

void wallet::transfer_migration(
    const std::vector<cryptonote::tx_destination_entry> &dsts, const crypto::hash bitcoin_transaction_hash,
    const size_t fake_outputs_count, const std::vector<size_t> &unused_transfers_indices,
    uint64_t unlock_time, uint64_t fee, const std::vector<uint8_t> &extra,
    cryptonote::transaction &tx, pending_tx &ptx, bool trusted_daemon)
{
  using namespace cryptonote;

  auto destination_split_strategy = detail::digit_split_strategy;
  const tx_dust_policy dust_policy = tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD);

  // throw if attempting a transaction with no destinations
  THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);

  THROW_WALLET_EXCEPTION_IF(m_multisig, error::wallet_internal_error, "Multisig is not supported");

  uint64_t upper_transaction_size_limit = get_upper_transaction_size_limit();
  uint64_t needed_money = fee;
  uint64_t needed_tokens = 0;

  // calculate total amount of safex cash being sent to all destinations
  // throw if total amount overflows uint64_t
  for(auto& dt: dsts)
  {
    THROW_WALLET_EXCEPTION_IF((!dt.token_transaction && 0 == dt.amount), error::zero_destination);
    THROW_WALLET_EXCEPTION_IF((dt.token_transaction && 0 == dt.token_amount), error::zero_destination);
    needed_money += dt.amount;
    needed_tokens += dt.token_amount;
    THROW_WALLET_EXCEPTION_IF(needed_money < dt.amount, error::tx_sum_overflow, dsts, fee, m_nettype);
    THROW_WALLET_EXCEPTION_IF(needed_tokens < dt.token_amount, error::tx_sum_overflow, dsts, fee, m_nettype);
  }

  // randomly select inputs for transaction
  // throw if requested send amount is greater than (unlocked) amount available to send
  std::vector<size_t> selected_transfers;
  uint64_t found_money = select_transfers(needed_money, unused_transfers_indices, selected_transfers, trusted_daemon);
  THROW_WALLET_EXCEPTION_IF(found_money < needed_money, error::not_enough_unlocked_cash, found_money, needed_money - fee, fee);

  uint32_t subaddr_account = m_transfers[*selected_transfers.begin()].m_subaddr_index.major;
  for (auto i = ++selected_transfers.begin(); i != selected_transfers.end(); ++i) {
    THROW_WALLET_EXCEPTION_IF(subaddr_account != m_transfers[*i].m_subaddr_index.major, error::wallet_internal_error, "the tx uses funds from multiple accounts");
  }


  typedef COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry out_entry;
  typedef cryptonote::tx_source_entry::output_entry tx_output_entry;

  COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response daemon_resp = AUTO_VAL_INIT(daemon_resp);
  if(fake_outputs_count)
  {
    COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request req = AUTO_VAL_INIT(req);
    req.output_type = cryptonote::tx_out_type::out_cash;
    req.outs_count = fake_outputs_count + 1;// add one to make possible (if need) to skip real output key
    for(size_t idx: selected_transfers)
    {
      const transfer_container::const_iterator it = m_transfers.begin() + idx;
      THROW_WALLET_EXCEPTION_IF(it->m_tx.vout.size() <= it->m_internal_output_index, error::wallet_internal_error,
          "m_internal_output_index = " + std::to_string(it->m_internal_output_index) +
          " is greater or equal to outputs count = " + std::to_string(it->m_tx.vout.size()));
      req.amounts.push_back(it->amount());
    }

    m_daemon_rpc_mutex.lock();
    bool r = epee::net_utils::invoke_http_bin("/getrandom_outs.bin", req, daemon_resp, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "getrandom_outs.bin");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "getrandom_outs.bin");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status != CORE_RPC_STATUS_OK, error::get_random_outs_error, daemon_resp.status);
    THROW_WALLET_EXCEPTION_IF(daemon_resp.outs.size() != selected_transfers.size(), error::wallet_internal_error,
        "daemon returned wrong response for getrandom_outs.bin, wrong amounts count = " +
        std::to_string(daemon_resp.outs.size()) + ", expected " +  std::to_string(selected_transfers.size()));

    std::unordered_map<uint64_t, uint64_t> scanty_outs;
    for(COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount& amount_outs: daemon_resp.outs)
    {
      if (amount_outs.outs.size() < fake_outputs_count)
      {
        scanty_outs[amount_outs.amount] = amount_outs.outs.size();
      }
    }
    THROW_WALLET_EXCEPTION_IF(!scanty_outs.empty(), error::not_enough_outs_to_mix, scanty_outs, fake_outputs_count);
  }

  //prepare inputs
  size_t i = 0;
  std::vector<cryptonote::tx_source_entry> sources;
  //Here, we select cash inputs
  for(size_t idx: selected_transfers)
  {
    sources.resize(sources.size()+1);
    cryptonote::tx_source_entry& src = sources.back();
    src = boost::value_initialized<cryptonote::tx_source_entry>();
    const transfer_details& td = m_transfers[idx];
    src.amount = td.amount();
    src.token_amount = 0;
    src.referenced_output_type = tx_out_type::out_cash;
    //paste mixin transaction
    if(!daemon_resp.outs.empty())
    {
      daemon_resp.outs[i].outs.sort([](const out_entry& a, const out_entry& b){return a.global_amount_index < b.global_amount_index;});
      for(out_entry& daemon_oe: daemon_resp.outs[i].outs)
      {
        if(td.m_global_output_index == daemon_oe.global_amount_index)
          continue;
        tx_output_entry oe = AUTO_VAL_INIT(oe);
        oe.first = daemon_oe.global_amount_index;
        oe.second.dest = rct::pk2rct(daemon_oe.out_key);
        oe.second.mask = rct::identity();
        src.outputs.push_back(oe);
        if(src.outputs.size() >= fake_outputs_count)
          break;
      }
    }

    //paste real transaction to the random index
    auto it_to_insert = std::find_if(src.outputs.begin(), src.outputs.end(), [&](const tx_output_entry &a)
    {
      return a.first >= td.m_global_output_index;
    });
    tx_output_entry real_oe = AUTO_VAL_INIT(real_oe);
    real_oe.first = td.m_global_output_index;
    real_oe.second.dest = rct::pk2rct(boost::get<txout_to_key>(td.m_tx.vout[td.m_internal_output_index].target).key);
    real_oe.second.mask = rct::identity();
    auto interted_it = src.outputs.insert(it_to_insert, real_oe);
    src.real_out_tx_key = get_tx_pub_key_from_extra(td.m_tx);
    src.real_output = interted_it - src.outputs.begin();
    src.real_output_in_tx_index = td.m_internal_output_index;
    detail::print_source_entry(src);
    ++i;
  }


  //add migration input, assume there is only one token migration destination!!
  for(auto& dt: dsts)
  {
    if (dt.token_transaction) {
      sources.resize(sources.size() + 1);
      cryptonote::tx_source_entry &src = sources.back();
      src = boost::value_initialized<cryptonote::tx_source_entry>();
      //Only migration account could sign txin_token_migration
      auto output = cryptonote::generate_migration_bitcoin_transaction_output(m_account.get_keys(), bitcoin_transaction_hash, dt.token_amount);
      src.outputs.push_back(output);
      src.token_amount = dt.token_amount;
      src.referenced_output_type = tx_out_type::out_bitcoin_migration;
      detail::print_token_source_entry(src);
    }
  }

  cryptonote::tx_destination_entry change_dts = AUTO_VAL_INIT(change_dts);
  cryptonote::tx_destination_entry change_token_dts = AUTO_VAL_INIT(change_token_dts);
  if (needed_money < found_money)
  {
    change_dts.addr = get_subaddress({subaddr_account, 0});
    change_dts.amount = found_money - needed_money;
  }

  std::vector<cryptonote::tx_destination_entry> splitted_dsts, dust_dsts;
  uint64_t dust = 0, token_dust = 0;
  destination_split_strategy(dsts, change_dts, change_token_dts, dust_policy.dust_threshold, splitted_dsts, dust_dsts);
  for(auto& d: dust_dsts) {
    THROW_WALLET_EXCEPTION_IF(dust_policy.dust_threshold < d.amount, error::wallet_internal_error, "invalid dust value: dust = " +
        std::to_string(d.amount) + ", dust_threshold = " + std::to_string(dust_policy.dust_threshold));
    THROW_WALLET_EXCEPTION_IF(dust_policy.dust_threshold < d.token_amount, error::wallet_internal_error, "invalid token dust value: dust = " +
        std::to_string(d.token_amount) + ", dust_threshold = " + std::to_string(dust_policy.dust_threshold));
  }
  for(auto& d: dust_dsts) {
    if ((!dust_policy.add_to_fee) && (!d.token_transaction))
      splitted_dsts.push_back(cryptonote::tx_destination_entry(d.amount, dust_policy.addr_for_dust, d.is_subaddress));

    if (d.token_transaction)
      splitted_dsts.push_back(cryptonote::tx_destination_entry(d.token_amount, dust_policy.addr_for_dust, d.is_subaddress, cryptonote::tx_out_type::out_token));

    dust += d.amount;
    token_dust += d.token_amount;
  }

  crypto::secret_key tx_key = AUTO_VAL_INIT(tx_key);
  std::vector<crypto::secret_key> additional_tx_keys;

  bool r = cryptonote::construct_tx_and_get_tx_key(m_account.get_keys(), m_subaddresses, sources, splitted_dsts, change_dts.addr, extra, tx, unlock_time, tx_key, additional_tx_keys);
  THROW_WALLET_EXCEPTION_IF(!r, error::tx_not_constructed, sources, splitted_dsts, unlock_time, m_nettype);
  THROW_WALLET_EXCEPTION_IF(upper_transaction_size_limit <= get_object_blobsize(tx), error::tx_too_big, tx, upper_transaction_size_limit);

  std::string key_images;
  bool all_are_valid_txin_types = std::all_of(tx.vin.begin(), tx.vin.end(), [&](const txin_v& s_e) -> bool
      {
        if ((s_e.type() != typeid(txin_to_key)) && (s_e.type() != typeid(txin_token_migration)) && (s_e.type() != typeid(txin_token_to_key))) {
          return false;
        }
        const crypto::key_image &k_image = *boost::apply_visitor(key_image_visitor(), s_e);
        key_images += boost::to_string(k_image) + " ";
        return true;
      });
  THROW_WALLET_EXCEPTION_IF(!all_are_valid_txin_types, error::unexpected_txin_type, tx);

  bool dust_sent_elsewhere = (dust_policy.addr_for_dust.m_view_public_key != change_dts.addr.m_view_public_key
      || dust_policy.addr_for_dust.m_spend_public_key != change_dts.addr.m_spend_public_key);

  if (dust_policy.add_to_fee || dust_sent_elsewhere) change_dts.amount -= dust;

  ptx.key_images = key_images;
  ptx.fee = (dust_policy.add_to_fee ? fee+dust : fee);
  ptx.dust = ((dust_policy.add_to_fee || dust_sent_elsewhere) ? dust : 0);
  ptx.dust_added_to_fee = dust_policy.add_to_fee;
  ptx.tx = tx;
  ptx.change_dts = change_dts;
  ptx.selected_transfers = selected_transfers;
  ptx.tx_key = tx_key;
  ptx.additional_tx_keys = additional_tx_keys;
  ptx.dests = dsts;
  ptx.construction_data.sources = sources;
  ptx.construction_data.change_dts = change_dts;
  ptx.construction_data.splitted_dsts = splitted_dsts;
  ptx.construction_data.selected_transfers = selected_transfers;
  ptx.construction_data.extra = tx.extra;
  ptx.construction_data.unlock_time = unlock_time;
  ptx.construction_data.use_rct = false;
  ptx.construction_data.dests = dsts;
  // record which subaddress indices are being used as inputs
  ptx.construction_data.subaddr_account = subaddr_account;
  ptx.construction_data.subaddr_indices.clear();
  for (size_t idx: selected_transfers)
    ptx.construction_data.subaddr_indices.insert(m_transfers[idx].m_subaddr_index.minor);
}

namespace {
// split_amounts(vector<cryptonote::tx_destination_entry> dsts, size_t num_splits)
//
// split amount for each dst in dsts into num_splits parts
// and make num_splits new vector<crypt...> instances to hold these new amounts
std::vector<std::vector<cryptonote::tx_destination_entry>> split_amounts(
    std::vector<cryptonote::tx_destination_entry> dsts, size_t num_splits)
{
  std::vector<std::vector<cryptonote::tx_destination_entry>> retVal;

  if (num_splits <= 1)
  {
    retVal.push_back(dsts);
    return retVal;
  }

  // for each split required
  for (size_t i=0; i < num_splits; i++)
  {
    std::vector<cryptonote::tx_destination_entry> new_dsts;

    // for each destination
    for (size_t j=0; j < dsts.size(); j++)
    {
      cryptonote::tx_destination_entry de;
      uint64_t amount;

      amount = dsts[j].amount;
      amount = amount / num_splits;

      // if last split, add remainder
      if (i + 1 == num_splits)
      {
        amount += dsts[j].amount % num_splits;
      }

      de.addr = dsts[j].addr;
      de.amount = amount;

      new_dsts.push_back(de);
    }

    retVal.push_back(new_dsts);
  }

  return retVal;
}
} // anonymous namespace
//----------------------------------------------------------------------------------------------------
crypto::hash wallet::get_payment_id(const pending_tx &ptx) const
{
  std::vector<tx_extra_field> tx_extra_fields;
  parse_tx_extra(ptx.tx.extra, tx_extra_fields); // ok if partially parsed
  tx_extra_nonce extra_nonce;
  crypto::hash payment_id = null_hash;
  if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
  {
    crypto::hash8 payment_id8 = null_hash8;
    if(get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
    {
      if (ptx.dests.empty())
      {
        MWARNING("Encrypted payment id found, but no destinations public key, cannot decrypt");
        return crypto::null_hash;
      }
      if (m_account.get_device().decrypt_payment_id(payment_id8, ptx.dests[0].addr.m_view_public_key, ptx.tx_key))
      {
        memcpy(payment_id.data, payment_id8.data, 8);
      }
    }
    else if (!get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
    {
      payment_id = crypto::null_hash;
    }
  }
  return payment_id;
}

//----------------------------------------------------------------------------------------------------
// take a pending tx and actually send it to the daemon
void wallet::commit_tx(pending_tx& ptx)
{
  using namespace cryptonote;

  if(m_light_wallet)
  {
    cryptonote::COMMAND_RPC_SUBMIT_RAW_TX::request oreq;
    cryptonote::COMMAND_RPC_SUBMIT_RAW_TX::response ores;
    oreq.address = get_account().get_public_address_str(m_nettype);
    oreq.view_key = string_tools::pod_to_hex(get_account().get_keys().m_view_secret_key);
    oreq.tx = epee::string_tools::buff_to_hex_nodelimer(tx_to_blob(ptx.tx));
    m_daemon_rpc_mutex.lock();
    bool r = epee::net_utils::invoke_http_json("/submit_raw_tx", oreq, ores, m_http_client, rpc_timeout, "POST");
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "submit_raw_tx");
    // MyMonero and OpenMonero use different status strings
    THROW_WALLET_EXCEPTION_IF(ores.status != "OK" && ores.status != "success" , error::tx_rejected, ptx.tx, ores.status, ores.error);
  }
  else
  {
    // Normal submit
    COMMAND_RPC_SEND_RAW_TX::request req;
    req.tx_as_hex = epee::string_tools::buff_to_hex_nodelimer(tx_to_blob(ptx.tx));
    req.do_not_relay = false;
    COMMAND_RPC_SEND_RAW_TX::response daemon_send_resp;
    m_daemon_rpc_mutex.lock();
    bool r = epee::net_utils::invoke_http_json("/sendrawtransaction", req, daemon_send_resp, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "sendrawtransaction");
    THROW_WALLET_EXCEPTION_IF(daemon_send_resp.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "sendrawtransaction");
    THROW_WALLET_EXCEPTION_IF(daemon_send_resp.status != CORE_RPC_STATUS_OK, error::tx_rejected, ptx.tx, daemon_send_resp.status, daemon_send_resp.reason);
    // sanity checks
    for (size_t idx: ptx.selected_transfers)
    {
      THROW_WALLET_EXCEPTION_IF(idx >= m_transfers.size(), error::wallet_internal_error,
          "Bad output index in selected transfers: " + boost::lexical_cast<std::string>(idx));
    }
  }
  crypto::hash txid;

  txid = get_transaction_hash(ptx.tx);
  crypto::hash payment_id = crypto::null_hash;
  std::vector<cryptonote::tx_destination_entry> dests;
  uint64_t amount_in = 0;
  uint64_t token_amount_in = 0;
  if (store_tx_info())
  {
    payment_id = get_payment_id(ptx);
    dests = ptx.dests;
    for(size_t idx: ptx.selected_transfers)
    {
      amount_in += m_transfers[idx].amount();
      token_amount_in += m_transfers[idx].token_amount();
    }
  }
  add_unconfirmed_tx(ptx.tx, amount_in, token_amount_in, dests, payment_id, ptx.change_dts.amount, ptx.change_token_dts.token_amount, ptx.construction_data.subaddr_account, ptx.construction_data.subaddr_indices);
  if (store_tx_info())
  {
    m_tx_keys.insert(std::make_pair(txid, ptx.tx_key));
    m_additional_tx_keys.insert(std::make_pair(txid, ptx.additional_tx_keys));
  }

  LOG_PRINT_L2("transaction " << txid << " generated ok and sent to daemon, key_images: [" << ptx.key_images << "]");

  for(size_t idx: ptx.selected_transfers)
  {
    set_spent(idx, 0);
  }

  // tx generated, get rid of used k values
  for (size_t idx: ptx.selected_transfers)
    m_transfers[idx].m_multisig_k.clear();

  //fee includes dust if dust policy specified it.
  LOG_PRINT_L1("Transaction successfully sent. <" << txid << ">" << ENDL
            << "Commission: " << print_money(ptx.fee) << " (dust sent to dust addr: " << print_money((ptx.dust_added_to_fee ? 0 : ptx.dust)) << ")" << ENDL
            << "Cash Balance: " << print_money(balance(ptx.construction_data.subaddr_account)) << " / Token Balance: " << print_money(token_balance(ptx.construction_data.subaddr_account)) << ENDL
            << "Cash Unlocked: " << print_money(unlocked_balance(ptx.construction_data.subaddr_account)) << " / Token Unlocked: " << print_money(unlocked_token_balance(ptx.construction_data.subaddr_account)) << ENDL
            << "Please, wait for confirmation for your balance to be unlocked.");
}

void wallet::commit_tx(std::vector<pending_tx>& ptx_vector)
{
  for (auto & ptx : ptx_vector)
  {
    commit_tx(ptx);
  }
}
//----------------------------------------------------------------------------------------------------
bool wallet::save_tx(const std::vector<pending_tx>& ptx_vector, const std::string &filename) const
{
  LOG_PRINT_L0("saving " << ptx_vector.size() << " transactions");
  unsigned_tx_set txs;
  for (auto &tx: ptx_vector)
  {
    // Short payment id is encrypted with tx_key.
    // Since sign_tx() generates new tx_keys and encrypts the payment id, we need to save the decrypted payment ID
    // Save tx construction_data to unsigned_tx_set
    txs.txes.push_back(get_construction_data_with_decrypted_short_payment_id(tx, m_account.get_device()));
  }

  txs.transfers = m_transfers;
  // save as binary
  std::ostringstream oss;
  boost::archive::portable_binary_oarchive ar(oss);
  try
  {
    ar << txs;
  }
  catch (...)
  {
    return false;
  }
  LOG_PRINT_L2("Saving unsigned tx data: " << oss.str());
  std::string ciphertext = encrypt_with_view_secret_key(oss.str());
  return epee::file_io_utils::save_string_to_file(filename, std::string(UNSIGNED_TX_PREFIX) + ciphertext);
}
//----------------------------------------------------------------------------------------------------
bool wallet::load_unsigned_tx(const std::string &unsigned_filename, unsigned_tx_set &exported_txs) const
{
  std::string s;
  boost::system::error_code errcode;

  if (!boost::filesystem::exists(unsigned_filename, errcode))
  {
    LOG_PRINT_L0("File " << unsigned_filename << " does not exist: " << errcode);
    return false;
  }
  if (!epee::file_io_utils::load_file_to_string(unsigned_filename.c_str(), s))
  {
    LOG_PRINT_L0("Failed to load from " << unsigned_filename);
    return false;
  }
  const size_t magiclen = strlen(UNSIGNED_TX_PREFIX) - 1;
  if (strncmp(s.c_str(), UNSIGNED_TX_PREFIX, magiclen))
  {
    LOG_PRINT_L0("Bad magic from " << unsigned_filename);
    return false;
  }
  s = s.substr(magiclen);
  const char version = s[0];
  s = s.substr(1);
  if (version == '\003')
  {
    try
    {
      std::istringstream iss(s);
      boost::archive::portable_binary_iarchive ar(iss);
      ar >> exported_txs;
    }
    catch (...)
    {
      LOG_PRINT_L0("Failed to parse data from " << unsigned_filename);
      return false;
    }
  }
  else if (version == '\004')
  {
    try
    {
      s = decrypt_with_view_secret_key(s);
      try
      {
        std::istringstream iss(s);
        boost::archive::portable_binary_iarchive ar(iss);
        ar >> exported_txs;
      }
      catch (...)
      {
        LOG_PRINT_L0("Failed to parse data from " << unsigned_filename);
        return false;
      }
    }
    catch (const std::exception &e)
    {
      LOG_PRINT_L0("Failed to decrypt " << unsigned_filename << ": " << e.what());
      return false;
    }
  }
  else
  {
    LOG_PRINT_L0("Unsupported version in " << unsigned_filename);
    return false;
  }
  LOG_PRINT_L1("Loaded tx unsigned data from binary: " << exported_txs.txes.size() << " transactions");

  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::sign_tx(const std::string &unsigned_filename, const std::string &signed_filename, std::vector<wallet::pending_tx> &txs, std::function<bool(const unsigned_tx_set&)> accept_func, bool export_raw)
{
  unsigned_tx_set exported_txs;
  if(!load_unsigned_tx(unsigned_filename, exported_txs))
    return false;

  if (accept_func && !accept_func(exported_txs))
  {
    LOG_PRINT_L1("Transactions rejected by callback");
    return false;
  }
  return sign_tx(exported_txs, signed_filename, txs, export_raw);
}

//----------------------------------------------------------------------------------------------------
bool wallet::sign_tx(unsigned_tx_set &exported_txs, const std::string &signed_filename, std::vector<wallet::pending_tx> &txs, bool export_raw)
{
  import_outputs(exported_txs.transfers);

  // sign the transactions
  signed_tx_set signed_txes;
  for (size_t n = 0; n < exported_txs.txes.size(); ++n)
  {
    tools::wallet::tx_construction_data &sd = exported_txs.txes[n];
    THROW_WALLET_EXCEPTION_IF(sd.sources.empty(), error::wallet_internal_error, "Empty sources");
    LOG_PRINT_L1(" " << (n+1) << ": " << sd.sources.size() << " inputs, ring size " << sd.sources[0].outputs.size());
    signed_txes.ptx.push_back(pending_tx());
    tools::wallet::pending_tx &ptx = signed_txes.ptx.back();
    crypto::secret_key tx_key;
    std::vector<crypto::secret_key> additional_tx_keys;
    bool r = cryptonote::construct_tx_and_get_tx_key(m_account.get_keys(), m_subaddresses, sd.sources, sd.splitted_dsts, sd.change_dts.addr, sd.extra, ptx.tx, sd.unlock_time, tx_key, additional_tx_keys);
    THROW_WALLET_EXCEPTION_IF(!r, error::tx_not_constructed, sd.sources, sd.splitted_dsts, sd.unlock_time, m_nettype);
    // we don't test tx size, because we don't know the current limit, due to not having a blockchain,
    // and it's a bit pointless to fail there anyway, since it'd be a (good) guess only. We sign anyway,
    // and if we really go over limit, the daemon will reject when it gets submitted. Chances are it's
    // OK anyway since it was generated in the first place, and rerolling should be within a few bytes.

    // normally, the tx keys are saved in commit_tx, when the tx is actually sent to the daemon.
    // we can't do that here since the tx will be sent from the compromised wallet, which we don't want
    // to see that info, so we save it here
    if (store_tx_info())
    {
      const crypto::hash txid = get_transaction_hash(ptx.tx);
      m_tx_keys.insert(std::make_pair(txid, tx_key));
      m_additional_tx_keys.insert(std::make_pair(txid, additional_tx_keys));
    }

    std::string key_images;
    bool all_are_valid_input = std::all_of(ptx.tx.vin.begin(), ptx.tx.vin.end(), [&](const txin_v& s_e) -> bool
    {
      if ((s_e.type() == typeid(txin_to_key)) || (s_e.type() == typeid(txin_token_to_key)) || (s_e.type() == typeid(txin_token_migration)))
      {
        const crypto::key_image &k_image = *boost::apply_visitor(key_image_visitor(), s_e);
        key_images += boost::to_string(k_image) + " ";
        return true;
      }
      else
        return false;
    });
    THROW_WALLET_EXCEPTION_IF(!all_are_valid_input, error::unexpected_txin_type, ptx.tx);

    ptx.key_images = key_images;
    ptx.fee = 0;
    for (const auto &i: sd.sources) ptx.fee += i.amount;
    for (const auto &i: sd.splitted_dsts) ptx.fee -= i.amount;
    ptx.dust = 0;
    ptx.dust_added_to_fee = false;
    ptx.change_dts = sd.change_dts;
    ptx.selected_transfers = sd.selected_transfers;
    ptx.tx_key = rct::rct2sk(rct::identity()); // don't send it back to the untrusted view wallet
    ptx.dests = sd.dests;
    ptx.construction_data = sd;

    txs.push_back(ptx);
  }

  // add key images
  signed_txes.key_images.resize(m_transfers.size());
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    if (!m_transfers[i].m_key_image_known || m_transfers[i].m_key_image_partial)
      LOG_PRINT_L0("WARNING: key image not known in signing wallet at index " << i);
    signed_txes.key_images[i] = m_transfers[i].m_key_image;
  }

  // save as binary
  std::ostringstream oss;
  boost::archive::portable_binary_oarchive ar(oss);
  try
  {
    ar << signed_txes;
  }
  catch(...)
  {
    return false;
  }
  LOG_PRINT_L3("Saving signed tx data (with encryption): " << oss.str());
  std::string ciphertext = encrypt_with_view_secret_key(oss.str());
  if (!epee::file_io_utils::save_string_to_file(signed_filename, std::string(SIGNED_TX_PREFIX) + ciphertext))
  {
    LOG_PRINT_L0("Failed to save file to " << signed_filename);
    return false;
  }
  // export signed raw tx without encryption
  if (export_raw)
  {
    for (size_t i = 0; i < signed_txes.ptx.size(); ++i)
    {
      std::string tx_as_hex = epee::string_tools::buff_to_hex_nodelimer(tx_to_blob(signed_txes.ptx[i].tx));
      std::string raw_filename = signed_filename + "_raw" + (signed_txes.ptx.size() == 1 ? "" : ("_" + std::to_string(i)));
      if (!epee::file_io_utils::save_string_to_file(raw_filename, tx_as_hex))
      {
        LOG_PRINT_L0("Failed to save file to " << raw_filename);
        return false;
      }
    }
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool wallet::load_tx(const std::string &signed_filename, std::vector<tools::wallet::pending_tx> &ptx, std::function<bool(const signed_tx_set&)> accept_func)
{
  std::string s;
  boost::system::error_code errcode;
  signed_tx_set signed_txs;

  if (!boost::filesystem::exists(signed_filename, errcode))
  {
    LOG_PRINT_L0("File " << signed_filename << " does not exist: " << errcode);
    return false;
  }

  if (!epee::file_io_utils::load_file_to_string(signed_filename.c_str(), s))
  {
    LOG_PRINT_L0("Failed to load from " << signed_filename);
    return false;
  }
  const size_t magiclen = strlen(SIGNED_TX_PREFIX) - 1;
  if (strncmp(s.c_str(), SIGNED_TX_PREFIX, magiclen))
  {
    LOG_PRINT_L0("Bad magic from " << signed_filename);
    return false;
  }
  s = s.substr(magiclen);
  const char version = s[0];
  s = s.substr(1);
  if (version == '\003')
  {
    try
    {
      std::istringstream iss(s);
      boost::archive::portable_binary_iarchive ar(iss);
      ar >> signed_txs;
    }
    catch (...)
    {
      LOG_PRINT_L0("Failed to parse data from " << signed_filename);
      return false;
    }
  }
  else if (version == '\004')
  {
    try
    {
      s = decrypt_with_view_secret_key(s);
      try
      {
        std::istringstream iss(s);
        boost::archive::portable_binary_iarchive ar(iss);
        ar >> signed_txs;
      }
      catch (...)
      {
        LOG_PRINT_L0("Failed to parse decrypted data from " << signed_filename);
        return false;
      }
    }
    catch (const std::exception &e)
    {
      LOG_PRINT_L0("Failed to decrypt " << signed_filename << ": " << e.what());
      return false;
    }
  }
  else
  {
    LOG_PRINT_L0("Unsupported version in " << signed_filename);
    return false;
  }
  LOG_PRINT_L0("Loaded signed tx data from binary: " << signed_txs.ptx.size() << " transactions");
  for (auto &ptx: signed_txs.ptx) LOG_PRINT_L0(cryptonote::obj_to_json_str(ptx.tx));

  if (accept_func && !accept_func(signed_txs))
  {
    LOG_PRINT_L1("Transactions rejected by callback");
    return false;
  }

  // import key images
  if (signed_txs.key_images.size() > m_transfers.size())
  {
    LOG_PRINT_L1("More key images returned that we know outputs for");
    return false;
  }
  for (size_t i = 0; i < signed_txs.key_images.size(); ++i)
  {
    transfer_details &td = m_transfers[i];
    if (td.m_key_image_known && !td.m_key_image_partial && td.m_key_image != signed_txs.key_images[i])
      LOG_PRINT_L0("WARNING: imported key image differs from previously known key image at index " << i << ": trusting imported one");
    td.m_key_image = signed_txs.key_images[i];
    m_key_images[m_transfers[i].m_key_image] = i;
    td.m_key_image_known = true;
    td.m_key_image_partial = false;
    m_pub_keys[m_transfers[i].get_public_key()] = i;
  }

  ptx = signed_txs.ptx;

  return true;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::get_fee_multiplier(uint32_t priority, int fee_algorithm) const
{
  static const uint64_t multipliers[3] = {1, 2, 3};

  if (fee_algorithm == -1)
    fee_algorithm = get_fee_algorithm();

  // 0 -> default (here, x1 till fee algorithm 2, x4 from it)
  if (priority == 0)
    priority = m_default_priority;
  if (priority == 0)
  {
    if (fee_algorithm >= 2)
      priority = 2;
    else
      priority = 1;
  }

  // 1 to 3 are allowed as priorities
  uint32_t max_priority = 3;
  if (priority >= 1 && priority <= max_priority)
  {
    switch (fee_algorithm)
    {
      case 0: return multipliers[priority-1];
      default: THROW_WALLET_EXCEPTION_IF (true, error::invalid_priority);
    }
  }

  THROW_WALLET_EXCEPTION_IF (false, error::invalid_priority);
  return 1;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::get_dynamic_per_kb_fee_estimate() const
{
  uint64_t fee;
  boost::optional<std::string> result = m_node_rpc_proxy.get_dynamic_per_kb_fee_estimate(FEE_ESTIMATE_GRACE_BLOCKS, fee);
  if (!result)
    return fee;
  LOG_PRINT_L1("Failed to query per kB fee, using " << print_money(FEE_PER_KB));
  return FEE_PER_KB;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::get_per_kb_fee() const
{
  if(m_light_wallet)
    return m_light_wallet_per_kb_fee;
  bool use_dyn_fee = use_fork_rules(HF_VERSION_DYNAMIC_FEE, -720 * 1);
  if (!use_dyn_fee)
    return FEE_PER_KB;

  return get_dynamic_per_kb_fee_estimate();
}
//----------------------------------------------------------------------------------------------------
int wallet::get_fee_algorithm() const
{
  return 0;
}
//------------------------------------------------------------------------------------------------------------------------------
uint64_t wallet::adjust_mixin(uint64_t mixin) const
{
  if (mixin < 2 && use_fork_rules(HF_VERSION_MIN_MIXIN_REQUIRED, 10)) {
    MWARNING("Requested ring size " << (mixin + 1) << " too low for hard fork 2, using 3");
    mixin = 2;
  }
  return mixin;
}
//----------------------------------------------------------------------------------------------------
uint32_t wallet::adjust_priority(uint32_t priority)
{
  if (priority == 0 && m_default_priority == 0 && auto_low_priority())
  {
    try
    {
      // check if there's a backlog in the tx pool
      const double fee_level = get_fee_multiplier(1) * get_per_kb_fee() * (12/(double)13) / (double)1024;
      const std::vector<std::pair<uint64_t, uint64_t>> blocks = estimate_backlog({std::make_pair(fee_level, fee_level)});
      if (blocks.size() != 1)
      {
        MERROR("Bad estimated backlog array size");
        return priority;
      }
      else if (blocks[0].first > 0)
      {
        MINFO("We don't use the low priority because there's a backlog in the tx pool.");
        return priority;
      }

      // get the current full reward zone
      cryptonote::COMMAND_RPC_GET_INFO::request getinfo_req = AUTO_VAL_INIT(getinfo_req);
      cryptonote::COMMAND_RPC_GET_INFO::response getinfo_res = AUTO_VAL_INIT(getinfo_res);
      m_daemon_rpc_mutex.lock();
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_info", getinfo_req, getinfo_res, m_http_client);
      m_daemon_rpc_mutex.unlock();
      THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_info");
      THROW_WALLET_EXCEPTION_IF(getinfo_res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_info");
      THROW_WALLET_EXCEPTION_IF(getinfo_res.status != CORE_RPC_STATUS_OK, error::get_tx_pool_error);
      const uint64_t full_reward_zone = getinfo_res.block_size_limit / 2;

      // get the last N block headers and sum the block sizes
      const size_t N = 10;
      if (m_blockchain.size() < N)
      {
        MERROR("The blockchain is too short");
        return priority;
      }
      cryptonote::COMMAND_RPC_GET_BLOCK_HEADERS_RANGE::request getbh_req = AUTO_VAL_INIT(getbh_req);
      cryptonote::COMMAND_RPC_GET_BLOCK_HEADERS_RANGE::response getbh_res = AUTO_VAL_INIT(getbh_res);
      m_daemon_rpc_mutex.lock();
      getbh_req.start_height = m_blockchain.size() - N;
      getbh_req.end_height = m_blockchain.size() - 1;
      r = net_utils::invoke_http_json_rpc("/json_rpc", "getblockheadersrange", getbh_req, getbh_res, m_http_client, rpc_timeout);
      m_daemon_rpc_mutex.unlock();
      THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "getblockheadersrange");
      THROW_WALLET_EXCEPTION_IF(getbh_res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "getblockheadersrange");
      THROW_WALLET_EXCEPTION_IF(getbh_res.status != CORE_RPC_STATUS_OK, error::get_blocks_error, getbh_res.status);
      if (getbh_res.headers.size() != N)
      {
        MERROR("Bad blockheaders size");
        return priority;
      }
      size_t block_size_sum = 0;
      for (const cryptonote::block_header_response &i : getbh_res.headers)
      {
        block_size_sum += i.block_size;
      }

      // estimate how 'full' the last N blocks are
      const size_t P = 100 * block_size_sum / (N * full_reward_zone);
      MINFO((boost::format("The last %d blocks fill roughly %d%% of the full reward zone.") % N % P).str());
      if (P > 80)
      {
        MINFO("We don't use the low priority because recent blocks are quite full.");
        return priority;
      }
      MINFO("We'll use the low priority because probably it's safe to do so.");
      return 1;
    }
    catch (const std::exception &e)
    {
      MERROR(e.what());
    }
  }
  return priority;
}
//----------------------------------------------------------------------------------------------------
// separated the call(s) to wallet::transfer into their own function
//
// this function will make multiple calls to wallet::transfer if multiple
// transactions will be required
std::vector<wallet::pending_tx> wallet::create_transactions(std::vector<cryptonote::tx_destination_entry> dsts, const size_t fake_outs_count, const uint64_t unlock_time, uint32_t priority, const std::vector<uint8_t>& extra, bool trusted_daemon)
{
  const std::vector<size_t> unused_transfers_indices = select_available_outputs_from_histogram(fake_outs_count + 1, true, true, true, trusted_daemon, cryptonote::tx_out_type::out_cash);

  const uint64_t fee_per_kb  = get_per_kb_fee();
  const uint64_t fee_multiplier = get_fee_multiplier(priority, get_fee_algorithm());

  // failsafe split attempt counter
  size_t attempt_count = 0;

  for(attempt_count = 1; ;attempt_count++)
  {
    size_t num_tx = 0.5 + pow(1.7,attempt_count-1);

    auto split_values = split_amounts(dsts, num_tx);

    // Throw if split_amounts comes back with a vector of size different than it should
    if (split_values.size() != num_tx)
    {
      throw std::runtime_error("Splitting transactions returned a number of potential tx not equal to what was requested");
    }

    std::vector<pending_tx> ptx_vector;
    try
    {
      // for each new destination vector (i.e. for each new tx)
      for (auto & dst_vector : split_values)
      {
        cryptonote::transaction tx;
        pending_tx ptx;

	// loop until fee is met without increasing tx size to next KB boundary.
	const size_t estimated_tx_size = estimate_tx_size(unused_transfers_indices.size(), fake_outs_count, dst_vector.size(), extra.size());
	uint64_t needed_fee = calculate_fee(fee_per_kb, estimated_tx_size, fee_multiplier);
	do
	{
	  transfer(dst_vector, fake_outs_count, unused_transfers_indices, unlock_time, needed_fee, extra, tx, ptx, trusted_daemon);
	  auto txBlob = t_serializable_object_to_blob(ptx.tx);
          needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
	} while (ptx.fee < needed_fee);

        ptx_vector.push_back(ptx);

        // mark transfers to be used as "spent"
        for(size_t idx: ptx.selected_transfers)
        {
          set_spent(idx, 0);
        }
      }

      // if we made it this far, we've selected our transactions.  committing them will mark them spent,
      // so this is a failsafe in case they don't go through
      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        for(size_t idx2: ptx.selected_transfers)
        {
          set_unspent(idx2);
        }

      }

      // if we made it this far, we're OK to actually send the transactions
      return ptx_vector;

    }
    // only catch this here, other exceptions need to pass through to the calling function
    catch (const tools::error::tx_too_big& e)
    {

      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        for(size_t idx2: ptx.selected_transfers)
        {
          set_unspent(idx2);
        }
      }

      if (attempt_count >= MAX_SPLIT_ATTEMPTS)
      {
        throw;
      }
    }
    catch (...)
    {
      // in case of some other exception, make sure any tx in queue are marked unspent again

      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        for(size_t idx2: ptx.selected_transfers)
        {
          set_unspent(idx2);
        }
      }

      throw;
    }
  }
}

bool wallet::set_ring_database(const std::string &filename)
{
  m_ring_database = filename;
  MINFO("ringdb path set to " << filename);
  m_ringdb.reset();
  if (!m_ring_database.empty())
  {
    try
    {
      cryptonote::block b;
      generate_genesis(b);
      m_ringdb.reset(new tools::ringdb(m_ring_database, epee::string_tools::pod_to_hex(get_block_hash(b))));
    }
    catch (const std::exception &e)
    {
      MERROR("Failed to initialize ringdb: " << e.what());
      m_ring_database = "";
      return false;
    }
  }
  return true;
}

bool wallet::add_rings(const crypto::chacha_key &key, const cryptonote::transaction_prefix &tx)
{
  if (!m_ringdb)
    return false;
  try { return m_ringdb->add_rings(key, tx); }
  catch (const std::exception &e) { return false; }
}

bool wallet::add_rings(const cryptonote::transaction_prefix &tx)
{
  crypto::chacha_key key;
  generate_chacha_key_from_secret_keys(key);
  try { return add_rings(key, tx); }
  catch (const std::exception &e) { return false; }
}

bool wallet::remove_rings(const cryptonote::transaction_prefix &tx)
{
  if (!m_ringdb)
    return false;
  crypto::chacha_key key;
  generate_chacha_key_from_secret_keys(key);
  try { return m_ringdb->remove_rings(key, tx); }
  catch (const std::exception &e) { return false; }
}

bool wallet::get_ring(const crypto::chacha_key &key, const crypto::key_image &key_image, std::vector<uint64_t> &outs)
{
  if (!m_ringdb)
    return false;
  try { return m_ringdb->get_ring(key, key_image, outs); }
  catch (const std::exception &e) { return false; }
}

bool wallet::get_rings(const crypto::hash &txid, std::vector<std::pair<crypto::key_image, std::vector<uint64_t>>> &outs)
{
  for (auto i: m_confirmed_txs)
  {
    if (txid == i.first)
    {
      for (const auto &x: i.second.m_rings)
        outs.push_back({x.first, cryptonote::relative_output_offsets_to_absolute(x.second)});
      return true;
    }
  }
  for (auto i: m_unconfirmed_txs)
  {
    if (txid == i.first)
    {
      for (const auto &x: i.second.m_rings)
        outs.push_back({x.first, cryptonote::relative_output_offsets_to_absolute(x.second)});
      return true;
    }
  }
  return false;
}

bool wallet::get_ring(const crypto::key_image &key_image, std::vector<uint64_t> &outs)
{
  crypto::chacha_key key;
  generate_chacha_key_from_secret_keys(key);

  try { return get_ring(key, key_image, outs); }
  catch (const std::exception &e) { return false; }
}

bool wallet::set_ring(const crypto::key_image &key_image, const std::vector<uint64_t> &outs, bool relative)
{
  if (!m_ringdb)
    return false;

  crypto::chacha_key key;
  generate_chacha_key_from_secret_keys(key);

  try { return m_ringdb->set_ring(key, key_image, outs, relative); }
  catch (const std::exception &e) { return false; }
}

bool wallet::find_and_save_rings(bool force)
{
  if (!force && m_ring_history_saved)
    return true;
  if (!m_ringdb)
    return false;

  COMMAND_RPC_GET_TRANSACTIONS::request req = AUTO_VAL_INIT(req);
  COMMAND_RPC_GET_TRANSACTIONS::response res = AUTO_VAL_INIT(res);

  MDEBUG("Finding and saving rings...");

  // get payments we made
  std::vector<crypto::hash> txs_hashes;
  std::list<std::pair<crypto::hash,wallet::confirmed_transfer_details>> payments;
  get_payments_out(payments, 0, std::numeric_limits<uint64_t>::max(), boost::none, std::set<uint32_t>());
  for (const std::pair<crypto::hash,wallet::confirmed_transfer_details> &entry: payments)
  {
    const crypto::hash &txid = entry.first;
    txs_hashes.push_back(txid);
  }

  MDEBUG("Found " << std::to_string(txs_hashes.size()) << " transactions");

  crypto::chacha_key key;
  generate_chacha_key_from_secret_keys(key);

  // get those transactions from the daemon
  static const size_t SLICE_SIZE = 200;
  for (size_t slice = 0; slice < txs_hashes.size(); slice += SLICE_SIZE)
  {
    req.decode_as_json = false;
    req.prune = true;
    req.txs_hashes.clear();
    size_t ntxes = slice + SLICE_SIZE > txs_hashes.size() ? txs_hashes.size() - slice : SLICE_SIZE;
    for (size_t s = slice; s < slice + ntxes; ++s)
      req.txs_hashes.push_back(epee::string_tools::pod_to_hex(txs_hashes[s]));
    bool r;
    {
      const boost::lock_guard<boost::mutex> lock{m_daemon_rpc_mutex};
      r = epee::net_utils::invoke_http_json("/gettransactions", req, res, m_http_client, rpc_timeout);
    }
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "gettransactions");
    THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "gettransactions");
    THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::wallet_internal_error, "gettransactions");
    THROW_WALLET_EXCEPTION_IF(res.txs.size() != req.txs_hashes.size(), error::wallet_internal_error,
      "daemon returned wrong response for gettransactions, wrong txs count = " +
      std::to_string(res.txs.size()) + ", expected " + std::to_string(req.txs_hashes.size()));

    MDEBUG("Scanning " << res.txs.size() << " transactions");
    THROW_WALLET_EXCEPTION_IF(slice + res.txs.size() > txs_hashes.size(), error::wallet_internal_error, "Unexpected tx array size");
    auto it = req.txs_hashes.begin();
    for (size_t i = 0; i < res.txs.size(); ++i, ++it)
    {
    const auto &tx_info = res.txs[i];
    THROW_WALLET_EXCEPTION_IF(tx_info.tx_hash != epee::string_tools::pod_to_hex(txs_hashes[slice + i]), error::wallet_internal_error, "Wrong txid received");
    THROW_WALLET_EXCEPTION_IF(tx_info.tx_hash != *it, error::wallet_internal_error, "Wrong txid received");
    cryptonote::blobdata bd;
    THROW_WALLET_EXCEPTION_IF(!epee::string_tools::parse_hexstr_to_binbuff(tx_info.as_hex, bd), error::wallet_internal_error, "failed to parse tx from hexstr");
    cryptonote::transaction tx;
    crypto::hash tx_hash, tx_prefix_hash;
    THROW_WALLET_EXCEPTION_IF(!cryptonote::parse_and_validate_tx_from_blob(bd, tx, tx_hash, tx_prefix_hash), error::wallet_internal_error, "failed to parse tx from blob");
    THROW_WALLET_EXCEPTION_IF(epee::string_tools::pod_to_hex(tx_hash) != tx_info.tx_hash, error::wallet_internal_error, "txid mismatch");
    THROW_WALLET_EXCEPTION_IF(!add_rings(key, tx), error::wallet_internal_error, "Failed to save ring");
    }
  }

  MINFO("Found and saved rings for " << txs_hashes.size() << " transactions");
  m_ring_history_saved = true;
  return true;
}

bool wallet::blackball_output(const crypto::public_key &output)
{
  if (!m_ringdb)
    return false;
  try { return m_ringdb->blackball(output); }
  catch (const std::exception &e) { return false; }
}

bool wallet::set_blackballed_outputs(const std::vector<crypto::public_key> &outputs, bool add)
{
  if (!m_ringdb)
    return false;
  try
  {
    bool ret = true;
    if (!add)
      ret &= m_ringdb->clear_blackballs();
    for (const auto &output: outputs)
      ret &= m_ringdb->blackball(output);
    return ret;
  }
  catch (const std::exception &e) { return false; }
}

bool wallet::unblackball_output(const crypto::public_key &output)
{
  if (!m_ringdb)
    return false;
  try { return m_ringdb->unblackball(output); }
  catch (const std::exception &e) { return false; }
}

bool wallet::is_output_blackballed(const crypto::public_key &output) const
{
  if (!m_ringdb)
    return false;
  try { return m_ringdb->blackballed(output); }
  catch (const std::exception &e) { return false; }
}

bool wallet::tx_add_fake_output(std::vector<std::vector<tools::wallet::get_outs_entry>> &outs, uint64_t global_index, const crypto::public_key& output_public_key, const rct::key& mask, uint64_t real_index, bool unlocked) const
{
  if (!unlocked) // don't add locked outs
    return false;
  if (global_index == real_index) // don't re-add real one
    return false;
  auto item = std::make_tuple(global_index, output_public_key, mask);
  CHECK_AND_ASSERT_MES(!outs.empty(), false, "internal error: outs is empty");
  if (std::find(outs.back().begin(), outs.back().end(), item) != outs.back().end()) // don't add duplicates
    return false;
  if (is_output_blackballed(output_public_key)) // don't add blackballed outputs
    return false;
  outs.back().push_back(item);
  return true;
}

void wallet::light_wallet_get_outs(std::vector<std::vector<tools::wallet::get_outs_entry>> &outs, const std::vector<size_t> &selected_transfers, size_t fake_outputs_count) {

  MDEBUG("LIGHTWALLET - Getting random outs");

  cryptonote::COMMAND_RPC_GET_RANDOM_OUTS::request oreq;
  cryptonote::COMMAND_RPC_GET_RANDOM_OUTS::response ores;

  size_t light_wallet_requested_outputs_count = (size_t)((fake_outputs_count + 1) * 1.5 + 1);

  // Amounts to ask for
  // MyMonero api handle amounts and fees as strings
  for(size_t idx: selected_transfers) {
    const uint64_t ask_amount = m_transfers[idx].is_rct() ? 0 : m_transfers[idx].amount();
    std::ostringstream amount_ss;
    amount_ss << ask_amount;
    oreq.amounts.push_back(amount_ss.str());
  }

  oreq.count = light_wallet_requested_outputs_count;
  m_daemon_rpc_mutex.lock();
  bool r = epee::net_utils::invoke_http_json("/get_random_outs", oreq, ores, m_http_client, rpc_timeout, "POST");
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_random_outs");
  THROW_WALLET_EXCEPTION_IF(ores.amount_outs.empty() , error::wallet_internal_error, "No outputs received from light wallet node. Error: " + ores.Error);

  // Check if we got enough outputs for each amount
  for(auto& out: ores.amount_outs) {
    const uint64_t out_amount = boost::lexical_cast<uint64_t>(out.amount);
    THROW_WALLET_EXCEPTION_IF(out.outputs.size() < light_wallet_requested_outputs_count , error::wallet_internal_error, "Not enough outputs for amount: " + boost::lexical_cast<std::string>(out.amount));
    MDEBUG(out.outputs.size() << " outputs for amount "+ boost::lexical_cast<std::string>(out.amount) + " received from light wallet node");
  }

  MDEBUG("selected transfers size: " << selected_transfers.size());

  for(size_t idx: selected_transfers)
  {
    // Create new index
    outs.push_back(std::vector<get_outs_entry>());
    outs.back().reserve(fake_outputs_count + 1);

    // add real output first
    const transfer_details &td = m_transfers[idx];
    const uint64_t amount = td.is_rct() ? 0 : td.amount();
    outs.back().push_back(std::make_tuple(td.m_global_output_index, td.get_public_key(), rct::commit(td.amount(), td.m_mask)));
    MDEBUG("added real output " << string_tools::pod_to_hex(td.get_public_key()));

    // Even if the lightwallet server returns random outputs, we pick them randomly.
    std::vector<size_t> order;
    order.resize(light_wallet_requested_outputs_count);
    for (size_t n = 0; n < order.size(); ++n)
      order[n] = n;
    std::shuffle(order.begin(), order.end(), std::default_random_engine(crypto::rand<unsigned>()));


    LOG_PRINT_L2("Looking for " << (fake_outputs_count+1) << " outputs with amounts " << print_money(td.is_rct() ? 0 : td.amount()));
    MDEBUG("OUTS SIZE: " << outs.back().size());
    for (size_t o = 0; o < light_wallet_requested_outputs_count && outs.back().size() < fake_outputs_count + 1; ++o)
    {
      // Random pick
      size_t i = order[o];

      // Find which random output key to use
      bool found_amount = false;
      size_t amount_key;
      for(amount_key = 0; amount_key < ores.amount_outs.size(); ++amount_key)
      {
        if(boost::lexical_cast<uint64_t>(ores.amount_outs[amount_key].amount) == amount) {
          found_amount = true;
          break;
        }
      }
      THROW_WALLET_EXCEPTION_IF(!found_amount , error::wallet_internal_error, "Outputs for amount " + boost::lexical_cast<std::string>(ores.amount_outs[amount_key].amount) + " not found" );

      LOG_PRINT_L2("Index " << i << "/" << light_wallet_requested_outputs_count << ": idx " << ores.amount_outs[amount_key].outputs[i].global_index << " (real " << td.m_global_output_index << "), unlocked " << "(always in light)" << ", key " << ores.amount_outs[0].outputs[i].public_key);

      // Convert light wallet string data to proper data structures
      crypto::public_key tx_public_key;
      rct::key mask = AUTO_VAL_INIT(mask); // decrypted mask - not used here
      rct::key rct_commit = AUTO_VAL_INIT(rct_commit);
      THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, ores.amount_outs[amount_key].outputs[i].public_key), error::wallet_internal_error, "Invalid public_key");
      string_tools::hex_to_pod(ores.amount_outs[amount_key].outputs[i].public_key, tx_public_key);
      const uint64_t global_index = ores.amount_outs[amount_key].outputs[i].global_index;
      if(!light_wallet_parse_rct_str(ores.amount_outs[amount_key].outputs[i].rct, tx_public_key, 0, mask, rct_commit, false))
        rct_commit = rct::zeroCommit(td.amount());

      if (tx_add_fake_output(outs, global_index, tx_public_key, rct_commit, td.m_global_output_index, true)) {
        MDEBUG("added fake output " << ores.amount_outs[amount_key].outputs[i].public_key);
        MDEBUG("index " << global_index);
      }
    }

    THROW_WALLET_EXCEPTION_IF(outs.back().size() < fake_outputs_count + 1 , error::wallet_internal_error, "Not enough fake outputs found" );

    // Real output is the first. Shuffle outputs
    MTRACE(outs.back().size() << " outputs added. Sorting outputs by index:");
    std::sort(outs.back().begin(), outs.back().end(), [](const get_outs_entry &a, const get_outs_entry &b) { return std::get<0>(a) < std::get<0>(b); });

    // Print output order
    for(auto added_out: outs.back())
      MTRACE(std::get<0>(added_out));

  }
}

void wallet::get_outs(std::vector<std::vector<tools::wallet::get_outs_entry>> &outs, const std::vector<size_t> &selected_transfers, size_t fake_outputs_count, cryptonote::tx_out_type out_type)
{
  LOG_PRINT_L2("fake_outputs_count: " << fake_outputs_count);
  outs.clear();

  if(m_light_wallet && fake_outputs_count > 0) {
    light_wallet_get_outs(outs, selected_transfers, fake_outputs_count);
    return;
  }

  crypto::chacha_key key;
  generate_chacha_key_from_secret_keys(key);

  if (fake_outputs_count > 0)
  {
    std::vector<size_t> cash_token_selected_transfers;

    for(size_t idx: selected_transfers)
    {
      if (m_transfers[idx].m_output_type == cryptonote::tx_out_type::out_safex_account) //no fake outputs count for accounts
        continue;

      cash_token_selected_transfers.push_back(idx);

    }


    uint64_t segregation_fork_height = get_segregation_fork_height();
    // check whether we're shortly after the fork
    uint64_t height;
    boost::optional<std::string> result = m_node_rpc_proxy.get_height(height);
    throw_on_rpc_response_error(result, "get_info");
    bool is_shortly_after_segregation_fork = height >= segregation_fork_height && height < segregation_fork_height + SEGREGATION_FORK_VICINITY;
    bool is_after_segregation_fork = height >= segregation_fork_height;

    // get histogram for the amounts we need
    cryptonote::COMMAND_RPC_GET_OUTPUT_HISTOGRAM::request req_t = AUTO_VAL_INIT(req_t);
    cryptonote::COMMAND_RPC_GET_OUTPUT_HISTOGRAM::response resp_t = AUTO_VAL_INIT(resp_t);
    m_daemon_rpc_mutex.lock();
    for(size_t idx: cash_token_selected_transfers)
    {

      if (out_type == tx_out_type::out_token)
        req_t.amounts.push_back(m_transfers[idx].token_amount());

      if (out_type == tx_out_type::out_cash)
        req_t.amounts.push_back(m_transfers[idx].amount());
    }
    std::sort(req_t.amounts.begin(), req_t.amounts.end());
    auto end = std::unique(req_t.amounts.begin(), req_t.amounts.end());
    req_t.amounts.resize(std::distance(req_t.amounts.begin(), end));
    req_t.unlocked = true;
    req_t.recent_cutoff = time(NULL) - RECENT_OUTPUT_ZONE;
    req_t.out_type = out_type;
    bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_output_histogram", req_t, resp_t, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "transfer_selected");
    THROW_WALLET_EXCEPTION_IF(resp_t.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_output_histogram");
    THROW_WALLET_EXCEPTION_IF(resp_t.status != CORE_RPC_STATUS_OK, error::get_histogram_error, resp_t.status);

    // if we want to segregate fake outs pre or post fork, get distribution
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> segregation_limit;
    if (is_after_segregation_fork && (m_segregate_pre_fork_outputs || m_key_reuse_mitigation2))
    {
      cryptonote::COMMAND_RPC_GET_OUTPUT_DISTRIBUTION::request req_t = AUTO_VAL_INIT(req_t);
      cryptonote::COMMAND_RPC_GET_OUTPUT_DISTRIBUTION::response resp_t = AUTO_VAL_INIT(resp_t);
      for(size_t idx: cash_token_selected_transfers)
      {
        if (out_type == tx_out_type::out_token)
          req_t.amounts.push_back(m_transfers[idx].token_amount());

        if (out_type == tx_out_type::out_cash)
          req_t.amounts.push_back(m_transfers[idx].amount());
      }
      std::sort(req_t.amounts.begin(), req_t.amounts.end());
      auto end = std::unique(req_t.amounts.begin(), req_t.amounts.end());
      req_t.amounts.resize(std::distance(req_t.amounts.begin(), end));
      req_t.from_height = std::max<uint64_t>(segregation_fork_height, RECENT_OUTPUT_BLOCKS) - RECENT_OUTPUT_BLOCKS;
      req_t.to_height = segregation_fork_height + 1;
      req_t.cumulative = true;
      req_t.out_type = out_type;
      m_daemon_rpc_mutex.lock();
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_output_distribution", req_t, resp_t, m_http_client, rpc_timeout * 1000);
      m_daemon_rpc_mutex.unlock();
      THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "transfer_selected");
      THROW_WALLET_EXCEPTION_IF(resp_t.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_output_distribution");
      THROW_WALLET_EXCEPTION_IF(resp_t.status != CORE_RPC_STATUS_OK, error::get_output_distribution, resp_t.status);

      // check we got all data
      for(size_t idx: cash_token_selected_transfers)
      {

        const uint64_t value_amount = (out_type == tx_out_type::out_token ? m_transfers[idx].token_amount() : m_transfers[idx].amount());
        if (value_amount == 0) continue; //skip this outputs
        bool found = false;
        for (const auto &d: resp_t.distributions)
        {
          if (d.amount == value_amount)
          {
            THROW_WALLET_EXCEPTION_IF(d.start_height > segregation_fork_height, error::get_output_distribution, "Distribution start_height too high");
            THROW_WALLET_EXCEPTION_IF(segregation_fork_height - d.start_height >= d.distribution.size(), error::get_output_distribution, "Distribution size too small");
            THROW_WALLET_EXCEPTION_IF(segregation_fork_height - RECENT_OUTPUT_BLOCKS - d.start_height >= d.distribution.size(), error::get_output_distribution, "Distribution size too small");
            THROW_WALLET_EXCEPTION_IF(segregation_fork_height <= RECENT_OUTPUT_BLOCKS, error::wallet_internal_error, "Fork height too low");
            THROW_WALLET_EXCEPTION_IF(segregation_fork_height - RECENT_OUTPUT_BLOCKS < d.start_height, error::get_output_distribution, "Bad start height");
            uint64_t till_fork = d.distribution[segregation_fork_height - d.start_height];
            uint64_t recent = till_fork - d.distribution[segregation_fork_height - RECENT_OUTPUT_BLOCKS - d.start_height];
            segregation_limit[value_amount] = std::make_pair(till_fork, recent);
            found = true;
            break;
          }
        }
        THROW_WALLET_EXCEPTION_IF(!found, error::get_output_distribution, "Requested amount not found in response");
      }
    }

    // we ask for more, to have spares if some outputs are still locked
    size_t base_requested_outputs_count = (size_t)((fake_outputs_count + 1) * 1.5 + 1);
    LOG_PRINT_L2("base_requested_outputs_count: " << base_requested_outputs_count);

    // generate output indices to request
    COMMAND_RPC_GET_OUTPUTS_BIN::request req = AUTO_VAL_INIT(req);
    COMMAND_RPC_GET_OUTPUTS_BIN::response daemon_resp = AUTO_VAL_INIT(daemon_resp);

    req.out_type = out_type;

    size_t num_selected_transfers = 0;
    for(size_t idx: cash_token_selected_transfers)
    {

      const transfer_details &td = m_transfers[idx];
      const uint64_t value_amount = (out_type == tx_out_type::out_token ? td.token_amount(): td.amount());
      if (value_amount == 0) continue;

      ++num_selected_transfers;
      std::unordered_set<uint64_t> seen_indices;
      // request more for rct in base recent (locked) coinbases are picked, since they're locked for longer
      size_t requested_outputs_count = base_requested_outputs_count + (td.is_rct() ? CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW - CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE : 0);
      size_t start = req.outputs.size();

      const bool output_is_pre_fork = td.m_block_height < segregation_fork_height;
      uint64_t num_outs = 0, num_recent_outs = 0;
      uint64_t num_post_fork_outs = 0;
      float pre_fork_num_out_ratio = 0.0f;
      float post_fork_num_out_ratio = 0.0f;

      if (is_after_segregation_fork && m_segregate_pre_fork_outputs && output_is_pre_fork)
      {
        num_outs = segregation_limit[value_amount].first;
        num_recent_outs = segregation_limit[value_amount].second;
      }
      else
      {
        // if there are just enough outputs to mix with, use all of them.
        // Eventually this should become impossible.
        for (const auto &he: resp_t.histogram)
        {
          if (he.amount == value_amount)
          {
            LOG_PRINT_L2("Found " << print_money(value_amount) << ": " << he.total_instances << " total, "
                << he.unlocked_instances << " unlocked, " << he.recent_instances << " recent");
            num_outs = he.unlocked_instances;
            num_recent_outs = he.recent_instances;
            break;
          }
        }
        if (is_after_segregation_fork && m_key_reuse_mitigation2)
        {
          if (output_is_pre_fork)
          {
            if (is_shortly_after_segregation_fork)
            {
              pre_fork_num_out_ratio = 33.4/100.0f * (1.0f - RECENT_OUTPUT_RATIO);
            }
            else
            {
              pre_fork_num_out_ratio = 33.4/100.0f * (1.0f - RECENT_OUTPUT_RATIO);
              post_fork_num_out_ratio = 33.4/100.0f * (1.0f - RECENT_OUTPUT_RATIO);
            }
          }
          else
          {
            if (is_shortly_after_segregation_fork)
            {
            }
            else
            {
              post_fork_num_out_ratio = 67.8/100.0f * (1.0f - RECENT_OUTPUT_RATIO);
            }
          }
        }
        num_post_fork_outs = num_outs - segregation_limit[value_amount].first;
      }

      LOG_PRINT_L1("" << num_outs << " unlocked outputs of size " << print_money(value_amount));
      THROW_WALLET_EXCEPTION_IF(num_outs == 0, error::wallet_internal_error,
          "histogram reports no unlocked outputs for " + boost::lexical_cast<std::string>(value_amount) + ", not even ours");
      THROW_WALLET_EXCEPTION_IF(num_recent_outs > num_outs, error::wallet_internal_error,
          "histogram reports more recent outs than outs for " + boost::lexical_cast<std::string>(value_amount));

      // how many fake outs to draw on a pre-fork triangular distribution
      size_t pre_fork_outputs_count = requested_outputs_count * pre_fork_num_out_ratio;
      size_t post_fork_outputs_count = requested_outputs_count * post_fork_num_out_ratio;
      // how many fake outs to draw otherwise
      size_t normal_output_count = requested_outputs_count - pre_fork_outputs_count - post_fork_outputs_count;

      // X% of those outs are to be taken from recent outputs
      size_t recent_outputs_count = normal_output_count * RECENT_OUTPUT_RATIO;
      if (recent_outputs_count == 0)
        recent_outputs_count = 1; // ensure we have at least one, if possible
      if (recent_outputs_count > num_recent_outs)
        recent_outputs_count = num_recent_outs;
      if (td.m_global_output_index >= num_outs - num_recent_outs && recent_outputs_count > 0)
        --recent_outputs_count; // if the real out is recent, pick one less recent fake out
      LOG_PRINT_L1("Fake output makeup: " << requested_outputs_count << " requested: " << recent_outputs_count << " recent, " <<
          pre_fork_outputs_count << " pre-fork, " << post_fork_outputs_count << " post-fork, " <<
          (requested_outputs_count - recent_outputs_count - pre_fork_outputs_count - post_fork_outputs_count) << " full-chain");

      uint64_t num_found = 0;

      // if we have a known ring, use it
      bool existing_ring_found = false;
      if (td.m_key_image_known && !td.m_key_image_partial)
      {
        std::vector<uint64_t> ring;
        if (get_ring(key, td.m_key_image, ring))
        {
          MINFO("This output has a known ring, reusing (size " << ring.size() << ")");
          THROW_WALLET_EXCEPTION_IF(ring.size() > fake_outputs_count + 1, error::wallet_internal_error,
              "An output in this transaction was previously spent on another chain with ring size " +
              std::to_string(ring.size()) + ", it cannot be spent now with ring size " +
              std::to_string(fake_outputs_count + 1) + " as it is smaller: use a higher ring size");
          bool own_found = false;
          existing_ring_found = true;
          for (const auto &out: ring)
          {
            MINFO("Ring has output " << out);
            if (out < num_outs)
            {
              MINFO("Using it");
              req.outputs.push_back({value_amount, out});
              ++num_found;
              seen_indices.emplace(out);
              if (out == td.m_global_output_index)
              {
                MINFO("This is the real output");
                own_found = true;
              }
            }
            else
            {
              MINFO("Ignoring output " << out << ", too recent");
            }
          }
          THROW_WALLET_EXCEPTION_IF(!own_found, error::wallet_internal_error,
              "Known ring does not include the spent output: " + std::to_string(td.m_global_output_index));
        }
      }

      if (num_outs <= requested_outputs_count && !existing_ring_found)
      {
        for (uint64_t i = 0; i < num_outs; i++)
          req.outputs.push_back({value_amount, i});
        // duplicate to make up shortfall: this will be caught after the RPC call,
        // so we can also output the amounts for which we can't reach the required
        // mixin after checking the actual unlockedness
        for (uint64_t i = num_outs; i < requested_outputs_count; ++i)
          req.outputs.push_back({value_amount, num_outs - 1});
      }
      else
      {
        // start with real one
        if (num_found == 0)
        {
          num_found = 1;
          seen_indices.emplace(td.m_global_output_index);
          req.outputs.push_back({value_amount, td.m_global_output_index});
          LOG_PRINT_L1("Selecting real output: " << td.m_global_output_index << " for " << print_money(value_amount));
        }

        // while we still need more mixins
        while (num_found < requested_outputs_count)
        {
          // if we've gone through every possible output, we've gotten all we can
          if (seen_indices.size() == num_outs)
            break;

          // get a random output index from the DB.  If we've already seen it,
          // return to the top of the loop and try again, otherwise add it to the
          // list of output indices we've seen.

          uint64_t i;
          const char *type = "";
          if (num_found - 1 < recent_outputs_count) // -1 to account for the real one we seeded with
          {
            // triangular distribution over [a,b) with a=0, mode c=b=up_index_limit
            uint64_t r = crypto::rand<uint64_t>() % ((uint64_t)1 << 53);
            double frac = std::sqrt((double)r / ((uint64_t)1 << 53));
            i = (uint64_t)(frac*num_recent_outs) + num_outs - num_recent_outs;
            // just in case rounding up to 1 occurs after calc
            if (i == num_outs)
              --i;
            type = "recent";
          }
          else if (num_found -1 < recent_outputs_count + pre_fork_outputs_count)
          {
            // triangular distribution over [a,b) with a=0, mode c=b=up_index_limit
            uint64_t r = crypto::rand<uint64_t>() % ((uint64_t)1 << 53);
            double frac = std::sqrt((double)r / ((uint64_t)1 << 53));
            i = (uint64_t)(frac*segregation_limit[value_amount].first);
            // just in case rounding up to 1 occurs after calc
            if (i == num_outs)
              --i;
            type = " pre-fork";
          }
          else if (num_found -1 < recent_outputs_count + pre_fork_outputs_count + post_fork_outputs_count)
          {
            // triangular distribution over [a,b) with a=0, mode c=b=up_index_limit
            uint64_t r = crypto::rand<uint64_t>() % ((uint64_t)1 << 53);
            double frac = std::sqrt((double)r / ((uint64_t)1 << 53));
            i = (uint64_t)(frac*num_post_fork_outs) + segregation_limit[value_amount].first;
            // just in case rounding up to 1 occurs after calc
            if (i == num_post_fork_outs+segregation_limit[value_amount].first)
              --i;
            type = "post-fork";
          }
          else
          {
            // triangular distribution over [a,b) with a=0, mode c=b=up_index_limit
            uint64_t r = crypto::rand<uint64_t>() % ((uint64_t)1 << 53);
            double frac = std::sqrt((double)r / ((uint64_t)1 << 53));
            i = (uint64_t)(frac*num_outs);
            // just in case rounding up to 1 occurs after calc
            if (i == num_outs)
              --i;
            type = "triangular";
          }

          if (seen_indices.count(i))
            continue;
          seen_indices.emplace(i);

          LOG_PRINT_L2("picking " << i << " as " << type);
          req.outputs.push_back({value_amount, i});
          ++num_found;
        }
      }

      // sort the subsection, to ensure the daemon doesn't know which output is ours
      std::sort(req.outputs.begin() + start, req.outputs.end(),
          [](const get_outputs_out &a, const get_outputs_out &b) { return a.index < b.index; });
    }

    for (auto i: req.outputs)
      LOG_PRINT_L1("asking for output " << i.index << " for " << print_money(i.amount));

    // get the keys for those
    m_daemon_rpc_mutex.lock();
    r = epee::net_utils::invoke_http_bin("/get_outs.bin", req, daemon_resp, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_outs.bin");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_outs.bin");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status != CORE_RPC_STATUS_OK, error::get_random_outs_error, daemon_resp.status);
    THROW_WALLET_EXCEPTION_IF(daemon_resp.outs.size() != req.outputs.size(), error::wallet_internal_error,
      "daemon returned wrong response for get_outs.bin, wrong amounts count = " +
      std::to_string(daemon_resp.outs.size()) + ", expected " +  std::to_string(req.outputs.size()));

    std::unordered_map<uint64_t, uint64_t> scanty_outs;
    size_t base = 0;
    outs.reserve(num_selected_transfers);
    for(size_t idx: cash_token_selected_transfers)
    {
      //skip cash outputs if getting token outputs or other way round
      if ((!m_transfers[idx].m_token_transfer && out_type == tx_out_type::out_token)
          || (m_transfers[idx].m_token_transfer && out_type == tx_out_type::out_cash))
        continue;

      const transfer_details &td = m_transfers[idx];
      size_t requested_outputs_count = base_requested_outputs_count + (td.is_rct() ? CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW - CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE : 0);
      outs.push_back(std::vector<get_outs_entry>());
      outs.back().reserve(fake_outputs_count + 1);
      const uint64_t value_amount = td.is_rct() ? 0 : (td.m_token_transfer ? td.token_amount(): td.amount());
      const rct::key mask = td.is_rct() ? rct::commit(value_amount, td.m_mask) : rct::zeroCommit(value_amount);

      uint64_t num_outs = 0;
      //const uint64_t amount = td.is_rct() ? 0 : td.amount();
      const bool output_is_pre_fork = td.m_block_height < segregation_fork_height;
      if (is_after_segregation_fork && m_segregate_pre_fork_outputs && output_is_pre_fork)
        num_outs = segregation_limit[value_amount].first;
      else for (const auto &he: resp_t.histogram)
      {
        if (he.amount == value_amount)
        {
          num_outs = he.unlocked_instances;
          break;
        }
      }

      // make sure the real outputs we asked for are really included, along
      // with the correct key and mask: this guards against an active attack
      // where the node sends dummy data for all outputs, and we then send
      // the real one, which the node can then tell from the fake outputs,
      // as it has different data than the dummy data it had sent earlier
      bool real_out_found = false;
      const crypto::public_key &pkey = *boost::apply_visitor(destination_public_key_visitor(), td.m_tx.vout[td.m_internal_output_index].target);
      for (size_t n = 0; n < requested_outputs_count; ++n)
      {
        size_t i = base + n;
        if (req.outputs[i].index == td.m_global_output_index)
        {
          if (daemon_resp.outs[i].key == pkey)
            if (daemon_resp.outs[i].mask == mask)
              real_out_found = true;
        }
      }
      THROW_WALLET_EXCEPTION_IF(!real_out_found, error::wallet_internal_error,
          "Daemon response did not include the requested real output");

      // pick real out first (it will be sorted when done)
      outs.back().push_back(std::make_tuple(td.m_global_output_index, pkey, mask));

      // then pick outs from an existing ring, if any
      bool existing_ring_found = false;
      if (td.m_key_image_known && !td.m_key_image_partial)
      {
        std::vector<uint64_t> ring;
        if (get_ring(key, td.m_key_image, ring))
        {
          for (uint64_t out: ring)
          {
            if (out < num_outs)
            {
              if (out != td.m_global_output_index)
              {
                bool found = false;
                for (size_t o = 0; o < requested_outputs_count; ++o)
                {
                  size_t i = base + o;
                  if (req.outputs[i].index == out)
                  {
                    LOG_PRINT_L2("Index " << i << "/" << requested_outputs_count << ": idx " << req.outputs[i].index << " (real " << td.m_global_output_index << "), unlocked " << daemon_resp.outs[i].unlocked << ", key " << daemon_resp.outs[i].key << " (from existing ring)");
                    tx_add_fake_output(outs, req.outputs[i].index, daemon_resp.outs[i].key, daemon_resp.outs[i].mask, td.m_global_output_index, daemon_resp.outs[i].unlocked);
                    found = true;
                    break;
                  }
                }
                THROW_WALLET_EXCEPTION_IF(!found, error::wallet_internal_error, "Falied to find existing ring output in daemon out data");
              }
            }
          }
        }
      }

      // then pick others in random order till we reach the required number
      // since we use an equiprobable pick here, we don't upset the triangular distribution
      std::vector<size_t> order;
      order.resize(requested_outputs_count);
      for (size_t n = 0; n < order.size(); ++n)
        order[n] = n;
      std::shuffle(order.begin(), order.end(), std::default_random_engine(crypto::rand<unsigned>()));

      LOG_PRINT_L2("Looking for " << (fake_outputs_count+1) << " outputs of size " << print_money(td.is_rct() ? 0 : (td.m_token_transfer ? td.token_amount(): td.amount())));
      for (size_t o = 0; o < requested_outputs_count && outs.back().size() < fake_outputs_count + 1; ++o)
      {
        size_t i = base + order[o];
        LOG_PRINT_L2("Index " << i << "/" << requested_outputs_count << ": idx " << req.outputs[i].index << " (real " << td.m_global_output_index << "), unlocked " << daemon_resp.outs[i].unlocked << ", key " << daemon_resp.outs[i].key);
        tx_add_fake_output(outs, req.outputs[i].index, daemon_resp.outs[i].key, daemon_resp.outs[i].mask, td.m_global_output_index, daemon_resp.outs[i].unlocked);
      }
      if (outs.back().size() < fake_outputs_count + 1)
      {
        scanty_outs[td.is_rct() ? 0 : (td.m_token_transfer ? td.token_amount(): td.amount())] = outs.back().size();
      }
      else
      {
        // sort the subsection, so any spares are reset in order
        std::sort(outs.back().begin(), outs.back().end(), [](const get_outs_entry &a, const get_outs_entry &b) { return std::get<0>(a) < std::get<0>(b); });
      }
      base += requested_outputs_count;
    }
    THROW_WALLET_EXCEPTION_IF(!scanty_outs.empty(), error::not_enough_outs_to_mix, scanty_outs, fake_outputs_count);
  }
  else
  {
    for (size_t idx: selected_transfers)
    {
      const transfer_details &td = m_transfers[idx];
      //skip cash outputs if getting token outputs or other way round
      if ((!m_transfers[idx].m_token_transfer && out_type == tx_out_type::out_token)
          || (m_transfers[idx].m_token_transfer && out_type == tx_out_type::out_cash)
          || (out_type == tx_out_type::out_safex_account && m_transfers[idx].m_output_type != out_type))
        continue;

      std::vector<get_outs_entry> v;
      const uint64_t value_amount = td.m_token_transfer ? td.token_amount(): td.amount();
      const rct::key mask = rct::zeroCommit(value_amount);
      v.push_back(std::make_tuple(td.m_global_output_index, td.get_public_key(), mask));
      outs.push_back(v);
    }
  }
}

template<typename T,  const cryptonote::tx_out_type OUT_TYPE>
void wallet::transfer_selected(const std::vector<cryptonote::tx_destination_entry>& dsts, const std::vector<size_t>& selected_transfers, size_t fake_outputs_count,
  std::vector<std::vector<tools::wallet::get_outs_entry>> &outs,
  uint64_t unlock_time, uint64_t fee, const std::vector<uint8_t>& extra, T destination_split_strategy, const tx_dust_policy& dust_policy, cryptonote::transaction& tx, pending_tx &ptx)
{
  using namespace cryptonote;
  // throw if attempting a transaction with no destinations
  THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);

  THROW_WALLET_EXCEPTION_IF(m_multisig, error::wallet_internal_error, "Multisig wallets not supported");

  uint64_t upper_transaction_size_limit = get_upper_transaction_size_limit();
  uint64_t needed_money = fee;
  uint64_t needed_tokens = 0;
  LOG_PRINT_L2("transfer: starting with fee " << print_money (needed_money));

  // calculate total amount being sent to all destinations
  // throw if total amount overflows uint64_t
  for(auto& dt: dsts)
  {
    THROW_WALLET_EXCEPTION_IF((!dt.token_transaction && 0 == dt.amount), error::zero_destination);
    THROW_WALLET_EXCEPTION_IF((dt.token_transaction && 0 == dt.token_amount), error::zero_destination);
    needed_money += dt.amount;
    needed_tokens += dt.token_amount;
    LOG_PRINT_L2("transfer: adding " << print_money(dt.token_amount) << " tokens, for a total of " << print_money (needed_tokens) <<
        " tokens and " << print_money(dt.amount) << " cash, for a total of " << print_money (needed_money) << " cash");
    THROW_WALLET_EXCEPTION_IF(needed_money < dt.amount, error::tx_sum_overflow, dsts, fee, m_nettype);
    THROW_WALLET_EXCEPTION_IF(needed_tokens < dt.token_amount, error::tx_sum_overflow, dsts, fee, m_nettype);
  }

  uint64_t found_money = 0;
  uint64_t found_tokens = 0;
  for(size_t idx: selected_transfers)
  {
    found_money += m_transfers[idx].amount();
    found_tokens += m_transfers[idx].token_amount();
  }

  LOG_PRINT_L2("wanted tokens:" << print_money(needed_tokens) << ", found tokens: " << print_money(found_tokens) << " wanted cash:" <<
      print_money(needed_money) << ", found cash:" << print_money(found_money) << ", fee " << print_money(fee) << " cash");
  THROW_WALLET_EXCEPTION_IF(found_money < needed_money, error::not_enough_unlocked_cash, found_money, needed_money - fee, fee);
  THROW_WALLET_EXCEPTION_IF(found_tokens < needed_tokens, error::not_enough_unlocked_tokens, found_tokens, needed_tokens);

  uint32_t subaddr_account = m_transfers[*selected_transfers.begin()].m_subaddr_index.major;
  for (auto i = ++selected_transfers.begin(); i != selected_transfers.end(); ++i)
    THROW_WALLET_EXCEPTION_IF(subaddr_account != m_transfers[*i].m_subaddr_index.major, error::wallet_internal_error, "the tx uses funds from multiple accounts");

  if (outs.empty())
    get_outs(outs, selected_transfers, fake_outputs_count, OUT_TYPE); // may throw

  if (OUT_TYPE == cryptonote::tx_out_type::out_token)
  {
    //find also outputs for cash fee payment in case of token transaction
    std::vector<std::vector<tools::wallet::get_outs_entry>> cash_fee_outs = AUTO_VAL_INIT(cash_fee_outs);
    get_outs(cash_fee_outs, selected_transfers, fake_outputs_count, cryptonote::tx_out_type::out_cash);
    for (auto out: cash_fee_outs)
      outs.push_back(out);
  }

  //prepare inputs
  LOG_PRINT_L2("preparing outputs");
  typedef cryptonote::tx_source_entry::output_entry tx_output_entry;
  size_t i = 0, out_index = 0;
  std::vector<cryptonote::tx_source_entry> sources;
  for(size_t idx: selected_transfers)
  {
    sources.resize(sources.size()+1);
    cryptonote::tx_source_entry& src = sources.back();
    const transfer_details& td = m_transfers[idx];
    src.amount = td.amount();
    src.token_amount = td.token_amount();
    src.referenced_output_type = (src.token_amount > 0) ? tx_out_type::out_token: tx_out_type::out_cash;
    //paste keys (fake and real)

    for (size_t n = 0; n < fake_outputs_count + 1; ++n)
    {
      tx_output_entry oe  = AUTO_VAL_INIT(oe);
      oe.first = std::get<0>(outs[out_index][n]);
      oe.second.dest = rct::pk2rct(std::get<1>(outs[out_index][n]));
      oe.second.mask = std::get<2>(outs[out_index][n]);

      src.outputs.push_back(oe);
      ++i;
    }

    //paste real transaction to the random index
    auto it_to_replace = std::find_if(src.outputs.begin(), src.outputs.end(), [&](const tx_output_entry& a)
    {
      return a.first == td.m_global_output_index;
    });
    THROW_WALLET_EXCEPTION_IF(it_to_replace == src.outputs.end(), error::wallet_internal_error,
        "real output not found");

    tx_output_entry real_oe = AUTO_VAL_INIT(real_oe);
    real_oe.first = td.m_global_output_index;
    real_oe.second.dest = rct::pk2rct(*boost::apply_visitor(destination_public_key_visitor(), td.m_tx.vout[td.m_internal_output_index].target));
    if (td.m_token_transfer)
      real_oe.second.mask = rct::commit(td.token_amount(), td.m_mask);
    else
      real_oe.second.mask = rct::commit(td.amount(), td.m_mask);
    *it_to_replace = real_oe;
    src.real_out_tx_key = get_tx_pub_key_from_extra(td.m_tx, td.m_pk_index);
    src.real_out_additional_tx_keys = get_additional_tx_pub_keys_from_extra(td.m_tx);
    src.real_output = it_to_replace - src.outputs.begin();
    src.real_output_in_tx_index = td.m_internal_output_index;
    detail::print_source_entry(src);
    ++out_index;
  }
  LOG_PRINT_L2("outputs prepared");

  cryptonote::tx_destination_entry change_dts = AUTO_VAL_INIT(change_dts);
  cryptonote::tx_destination_entry change_token_dts = AUTO_VAL_INIT(change_token_dts);

  if (needed_money < found_money)
  {
    change_dts.addr = get_subaddress({subaddr_account, 0});
    change_dts.amount = found_money - needed_money;
  }

  if (needed_tokens < found_tokens)
  {
    change_token_dts.addr = get_subaddress({subaddr_account, 0});
    change_token_dts.token_amount = found_tokens - needed_tokens;
  }

  std::vector<cryptonote::tx_destination_entry> splitted_dsts, dust_dsts;
  uint64_t dust = 0, token_dust = 0;
  destination_split_strategy(dsts, change_dts, change_token_dts, dust_policy.dust_threshold, splitted_dsts, dust_dsts);
  for(auto& d: dust_dsts) {
    THROW_WALLET_EXCEPTION_IF(dust_policy.dust_threshold < d.amount, error::wallet_internal_error, "invalid dust value: dust = " +
      std::to_string(d.amount) + ", dust_threshold = " + std::to_string(dust_policy.dust_threshold));
    THROW_WALLET_EXCEPTION_IF(dust_policy.dust_threshold < d.token_amount, error::wallet_internal_error, "invalid token dust value: dust = " +
            std::to_string(d.token_amount) + ", dust_threshold = " + std::to_string(dust_policy.dust_threshold));
  }
  for(auto& d: dust_dsts) {
    if ((!dust_policy.add_to_fee) && (!d.token_transaction))
      splitted_dsts.push_back(cryptonote::tx_destination_entry(d.amount, dust_policy.addr_for_dust, d.is_subaddress, cryptonote::tx_out_type::out_cash));

    if (d.token_transaction)
      splitted_dsts.push_back(cryptonote::tx_destination_entry(d.token_amount, dust_policy.addr_for_dust, d.is_subaddress, cryptonote::tx_out_type::out_token));

    dust += d.amount;
    token_dust += d.token_amount;
  }

  crypto::secret_key tx_key = AUTO_VAL_INIT(tx_key);
  std::vector<crypto::secret_key> additional_tx_keys;
  LOG_PRINT_L2("constructing tx");
  bool r = cryptonote::construct_tx_and_get_tx_key(m_account.get_keys(), m_subaddresses, sources, splitted_dsts, change_dts.addr, extra, tx, unlock_time, tx_key, additional_tx_keys);
  LOG_PRINT_L2("constructed tx, r="<<r);
  THROW_WALLET_EXCEPTION_IF(!r, error::tx_not_constructed, sources, splitted_dsts, unlock_time, m_nettype);
  THROW_WALLET_EXCEPTION_IF(upper_transaction_size_limit <= get_object_blobsize(tx), error::tx_too_big, tx, upper_transaction_size_limit);

  std::string key_images;
  bool all_are_valid_input = std::all_of(tx.vin.begin(), tx.vin.end(), [&](const txin_v& s_e) -> bool
  {
    if ((s_e.type() == typeid(txin_to_key)) || (s_e.type() == typeid(txin_token_to_key)))
    {
      const crypto::key_image &k_image = *boost::apply_visitor(key_image_visitor(), s_e);
      key_images += boost::to_string(k_image) + " ";
      return true;
    }
    else
      return false;
  });
  THROW_WALLET_EXCEPTION_IF(!all_are_valid_input, error::unexpected_txin_type, tx);


  bool dust_sent_elsewhere = (dust_policy.addr_for_dust.m_view_public_key != change_dts.addr.m_view_public_key
                                || dust_policy.addr_for_dust.m_spend_public_key != change_dts.addr.m_spend_public_key);

  if (dust_policy.add_to_fee || dust_sent_elsewhere) change_dts.amount -= dust;

  ptx.key_images = key_images;
  ptx.fee = (dust_policy.add_to_fee ? fee+dust : fee);
  ptx.dust = ((dust_policy.add_to_fee || dust_sent_elsewhere) ? dust : 0);
  ptx.dust_added_to_fee = dust_policy.add_to_fee;
  ptx.tx = tx;
  ptx.change_dts = change_dts;
  ptx.change_token_dts = change_token_dts;
  ptx.selected_transfers = selected_transfers;
  ptx.tx_key = tx_key;
  ptx.additional_tx_keys = additional_tx_keys;
  ptx.dests = dsts;
  ptx.construction_data.sources = sources;
  ptx.construction_data.change_dts = change_dts;
  ptx.construction_data.splitted_dsts = splitted_dsts;
  ptx.construction_data.selected_transfers = selected_transfers;
  ptx.construction_data.extra = tx.extra;
  ptx.construction_data.unlock_time = unlock_time;
  ptx.construction_data.use_rct = false;
  ptx.construction_data.dests = dsts;
  // record which subaddress indices are being used as inputs
  ptx.construction_data.subaddr_account = subaddr_account;
  ptx.construction_data.subaddr_indices.clear();
  for (size_t idx: selected_transfers)
    ptx.construction_data.subaddr_indices.insert(m_transfers[idx].m_subaddr_index.minor);
  LOG_PRINT_L2("transfer_selected done");
}


template<typename T>
void wallet::transfer_advanced(safex::command_t command_type, const std::vector<cryptonote::tx_destination_entry>& dsts, const std::vector<size_t>& selected_transfers,
        size_t fake_outputs_count, std::vector<std::vector<tools::wallet::get_outs_entry>> &outs,
        uint64_t unlock_time, uint64_t fee, const std::vector<uint8_t>& extra, T destination_split_strategy, const tx_dust_policy& dust_policy,
        cryptonote::transaction& tx, pending_tx &ptx, const safex::safex_account &sfx_acc)
{
  using namespace cryptonote;
  // throw if attempting a transaction with no destinations
  THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);

  THROW_WALLET_EXCEPTION_IF(m_multisig, error::wallet_internal_error, "Multisig wallets not supported");

  uint64_t upper_transaction_size_limit = get_upper_transaction_size_limit();
  uint64_t needed_money = fee;
  uint64_t needed_tokens = 0;
  uint64_t needed_staked_tokens = 0;
  LOG_PRINT_L2("transfer: starting with fee " << print_money (needed_money));

  safex::safex_account_keys my_safex_keys = AUTO_VAL_INIT(my_safex_keys);

  // calculate total amount being sent to all destinations
  // throw if total amount overflows uint64_t
  for(auto& dt: dsts)
  {
    needed_money += dt.amount;
    if (command_type == safex::command_t::token_unstake)
    {
      needed_staked_tokens += dt.token_amount;
      THROW_WALLET_EXCEPTION_IF(needed_staked_tokens <  dt.token_amount, error::tx_sum_overflow, dsts, fee, m_nettype);
    }
    else
    {
      needed_tokens += dt.token_amount;
      THROW_WALLET_EXCEPTION_IF(needed_tokens < dt.token_amount, error::tx_sum_overflow, dsts, fee, m_nettype);
    }

    LOG_PRINT_L2("advanced transfer: adding " << print_money(dt.token_amount) << " tokens, for a total of " << print_money (needed_tokens) <<
                                     " tokens and " << print_money(dt.amount) << " cash, for a total of " << print_money (needed_money) << " cash");
    THROW_WALLET_EXCEPTION_IF(needed_money < dt.amount, error::tx_sum_overflow, dsts, fee, m_nettype);

  }

  uint64_t found_money = 0;
  uint64_t found_tokens = 0;
  uint64_t found_staked_tokens = 0;
  for(size_t idx: selected_transfers)
  {
    found_money += m_transfers[idx].amount();
    found_tokens += m_transfers[idx].m_output_type == tx_out_type::out_token ? m_transfers[idx].token_amount() : 0;
    found_staked_tokens += m_transfers[idx].m_output_type == tx_out_type::out_staked_token ? m_transfers[idx].token_amount() : 0;
  }

  LOG_PRINT_L2("wanted tokens:" << print_money(needed_tokens) << ", found tokens: " << print_money(found_tokens) << " wanted cash:" <<
                                print_money(needed_money) << ", found cash:" << print_money(found_money) << ", fee " << print_money(fee) << " cash");
  THROW_WALLET_EXCEPTION_IF(found_money < needed_money, error::not_enough_unlocked_cash, found_money, needed_money - fee, fee);
  THROW_WALLET_EXCEPTION_IF(found_tokens < needed_tokens, error::not_enough_unlocked_tokens, found_tokens, needed_tokens);
  THROW_WALLET_EXCEPTION_IF(found_staked_tokens < needed_staked_tokens, error::not_enough_unlocked_staked_tokens, found_staked_tokens, needed_staked_tokens);

  uint32_t subaddr_account = m_transfers[*selected_transfers.begin()].m_subaddr_index.major;
  for (auto i = ++selected_transfers.begin(); i != selected_transfers.end(); ++i)
    THROW_WALLET_EXCEPTION_IF(subaddr_account != m_transfers[*i].m_subaddr_index.major, error::wallet_internal_error, "the tx uses funds from multiple accounts");

  if (outs.empty()) {

    if ((command_type == safex::command_t::token_stake || command_type == safex::command_t::create_account))
      get_outs(outs, selected_transfers, fake_outputs_count, tx_out_type::out_token); // may throw
    else if (command_type == safex::command_t::donate_network_fee || command_type == safex::command_t::simple_purchase)
      get_outs(outs, selected_transfers, fake_outputs_count, tx_out_type::out_cash); // may throw
    else if (command_type == safex::command_t::token_unstake)
      get_outs(outs, selected_transfers, fake_outputs_count, tx_out_type::out_staked_token); // may throw
    else if (command_type == safex::command_t::edit_account)
      get_outs(outs, selected_transfers, 0, tx_out_type::out_safex_account); // may throw
  }


  if ((command_type == safex::command_t::token_stake) || (command_type == safex::command_t::token_unstake)
      || (command_type == safex::command_t::create_account) || (command_type == safex::command_t::edit_account))
  {
    //find also outputs for cash fee payment in case of token transaction
    std::vector<std::vector<tools::wallet::get_outs_entry>> cash_fee_outs = AUTO_VAL_INIT(cash_fee_outs);
    get_outs(cash_fee_outs, selected_transfers, fake_outputs_count, cryptonote::tx_out_type::out_cash);
    for (auto out: cash_fee_outs)
      outs.push_back(out);
  }
  else if (command_type == safex::command_t::donate_network_fee || command_type == safex::command_t::simple_purchase)
  {
    //do nothing
  }
  else {
    THROW_WALLET_EXCEPTION(tools::error::command_not_supported);
  }

  //prepare inputs
  LOG_PRINT_L2("preparing outputs");
  typedef cryptonote::tx_source_entry::output_entry tx_output_entry;
  size_t i = 0, out_index = 0;
  std::vector<cryptonote::tx_source_entry> sources;
  std::vector<cryptonote::tx_source_entry> additional_sources; //advanced command inputs

  auto find_matching_advanced_output = [&](const tx_out_type out_type) {
    for (const cryptonote::tx_destination_entry& dt: dsts )
      if (dt.output_type == out_type) return dt;

    THROW_WALLET_EXCEPTION(tools::error::safex_missing_outputs_error);
    return cryptonote::tx_destination_entry{};
  };

  bool command_input_creted = false;
  for(size_t idx: selected_transfers)
  {
    sources.resize(sources.size() + 1);
    cryptonote::tx_source_entry &src = sources.back();
    const transfer_details &td = m_transfers[idx];
    src.amount = td.amount();
    src.token_amount = td.token_amount();
    if (td.get_out_type() != tx_out_type::out_invalid && td.get_out_type() != tx_out_type::out_cash)
      src.referenced_output_type = td.get_out_type();
    else
      src.referenced_output_type = (src.token_amount > 0) ? tx_out_type::out_token : tx_out_type::out_cash;

    //paste keys (fake and real)
    const size_t fake_outputs_count_revised = src.referenced_output_type == tx_out_type::out_safex_account ? 0 : fake_outputs_count;
    for (size_t n = 0; n < fake_outputs_count_revised + 1; ++n)
    {
      tx_output_entry oe = AUTO_VAL_INIT(oe);
      oe.first = std::get<0>(outs[out_index][n]);
      oe.second.dest = rct::pk2rct(std::get<1>(outs[out_index][n]));
      oe.second.mask = std::get<2>(outs[out_index][n]);

      src.outputs.push_back(oe);
      ++i;
    }

    //paste real transaction to the random index
    auto it_to_replace = std::find_if(src.outputs.begin(), src.outputs.end(), [&](const tx_output_entry &a)
    {
      return a.first == td.m_global_output_index;
    });
    THROW_WALLET_EXCEPTION_IF(it_to_replace == src.outputs.end(), error::wallet_internal_error,
                              "real output not found");

    tx_output_entry real_oe = AUTO_VAL_INIT(real_oe);
    real_oe.first = td.m_global_output_index;
    real_oe.second.dest = rct::pk2rct(*boost::apply_visitor(destination_public_key_visitor(), td.m_tx.vout[td.m_internal_output_index].target));
    *it_to_replace = real_oe;
    src.real_out_tx_key = get_tx_pub_key_from_extra(td.m_tx, td.m_pk_index);
    src.real_out_additional_tx_keys = get_additional_tx_pub_keys_from_extra(td.m_tx);
    src.real_output = it_to_replace - src.outputs.begin();
    src.real_output_in_tx_index = td.m_internal_output_index;

    //set command type
    if (command_type == safex::command_t::token_stake && src.referenced_output_type == tx_out_type::out_token)
      src.command_type = safex::command_t::token_stake;
    else if ((command_type == safex::command_t::simple_purchase || command_type == safex::command_t::donate_network_fee) && src.referenced_output_type == tx_out_type::out_cash)
      src.command_type = safex::command_t::donate_network_fee;
    else if (command_type == safex::command_t::token_unstake && src.referenced_output_type == tx_out_type::out_staked_token)
    {
      src.command_type = safex::command_t::token_unstake;

      //also, create additional source for interest distribution
      cryptonote::tx_source_entry src_interest = AUTO_VAL_INIT(src_interest);
      src_interest.command_type = safex::command_t::distribute_network_fee;
      src_interest.referenced_output_type = tx_out_type::out_network_fee;
      src_interest.real_output_in_tx_index = src.real_output_in_tx_index; //reference same token output
      //******************************************************************************************************/
      //todo atana check if this is safe, if we can use same public key for interest, as ring size is only 1
      //******************************************************************************************************/
      src_interest.real_out_tx_key = src.real_out_tx_key; // here just for completion, does not actually used for check
      src_interest.outputs = src.outputs;
      src_interest.real_output = src.real_output;
      src_interest.amount = get_interest_for_transfer(td);

      // add source and destinations
      if (src_interest.amount > 0)
      {
        additional_sources.push_back(src_interest);
      }
    }
    else if (command_type == safex::command_t::create_account && (!command_input_creted))
    {
      const cryptonote::tx_destination_entry &dt_account = find_matching_advanced_output(tx_out_type::out_safex_account);
      src.command_safex_data = dt_account.output_data;
      src.command_type = safex::command_t::create_account;
      command_input_creted = true;
      bool res = get_safex_account_keys(sfx_acc.username, my_safex_keys);
      THROW_WALLET_EXCEPTION_IF(!res, error::wallet_internal_error, "safex account keys missing");
    }
    else if (command_type == safex::command_t::edit_account && m_transfers[idx].m_output_type == tx_out_type::out_safex_account)
    {
      const cryptonote::tx_destination_entry &dt_account = find_matching_advanced_output(tx_out_type::out_safex_account_update);
      src.command_safex_data = dt_account.output_data;
      src.command_type = safex::command_t::edit_account;
      bool res = get_safex_account_keys(sfx_acc.username, my_safex_keys);
      THROW_WALLET_EXCEPTION_IF(!res, error::wallet_internal_error, "safex account keys missing");
    }



    detail::print_source_entry(src);
    ++out_index;
  }

  if (additional_sources.size() > 0) std::copy(additional_sources.begin(), additional_sources.end(), std::back_inserter(sources));

  LOG_PRINT_L2("outputs prepared");

  cryptonote::tx_destination_entry change_dts = AUTO_VAL_INIT(change_dts);
  cryptonote::tx_destination_entry change_token_dts = AUTO_VAL_INIT(change_token_dts);

  if (needed_money < found_money)
  {
    change_dts.addr = get_subaddress({subaddr_account, 0});
    change_dts.amount = found_money - needed_money;
  }

  if (needed_tokens < found_tokens)
  {
    change_token_dts.addr = get_subaddress({subaddr_account, 0});
    change_token_dts.token_amount = found_tokens - needed_tokens;
  }

  std::vector<cryptonote::tx_destination_entry> splitted_dsts, dust_dsts;
  uint64_t dust = 0;
  destination_split_strategy(dsts, change_dts, change_token_dts, dust_policy.dust_threshold, splitted_dsts, dust_dsts);
  for(auto& d: dust_dsts) {
    THROW_WALLET_EXCEPTION_IF(dust_policy.dust_threshold < d.amount, error::wallet_internal_error, "invalid dust value: dust = " +
                                                                                                   std::to_string(d.amount) + ", dust_threshold = " + std::to_string(dust_policy.dust_threshold));
    THROW_WALLET_EXCEPTION_IF(dust_policy.dust_threshold < d.token_amount, error::wallet_internal_error, "invalid token dust value: dust = " +
                                                                                                         std::to_string(d.token_amount) + ", dust_threshold = " + std::to_string(dust_policy.dust_threshold));
  }
  for(auto& d: dust_dsts) {
    if ((!dust_policy.add_to_fee))
      splitted_dsts.push_back(cryptonote::tx_destination_entry(d.amount, dust_policy.addr_for_dust, d.is_subaddress, cryptonote::tx_out_type::out_cash));

    dust += d.amount;
  }

  crypto::secret_key tx_key = AUTO_VAL_INIT(tx_key);
  std::vector<crypto::secret_key> additional_tx_keys;
  LOG_PRINT_L2("constructing tx");



  bool r = cryptonote::construct_tx_and_get_tx_key(m_account.get_keys(), m_subaddresses, sources, splitted_dsts, change_dts.addr, extra, tx, unlock_time, tx_key, additional_tx_keys, my_safex_keys);
  LOG_PRINT_L2("constructed tx, r="<<r);
  THROW_WALLET_EXCEPTION_IF(!r, error::tx_not_constructed, sources, splitted_dsts, unlock_time, m_nettype);
  THROW_WALLET_EXCEPTION_IF(upper_transaction_size_limit <= get_object_blobsize(tx), error::tx_too_big, tx, upper_transaction_size_limit);

  std::string key_images;
  bool all_are_valid_input = std::all_of(tx.vin.begin(), tx.vin.end(), [&](const txin_v& s_e) -> bool
  {
    if ((s_e.type() == typeid(txin_to_key)) || (s_e.type() == typeid(txin_token_to_key)) || (s_e.type() == typeid(txin_to_script)) )
    {
      const crypto::key_image &k_image = *boost::apply_visitor(key_image_visitor(), s_e);
      key_images += boost::to_string(k_image) + " ";
      return true;
    }
    else
      return false;
  });
  THROW_WALLET_EXCEPTION_IF(!all_are_valid_input, error::unexpected_txin_type, tx);


  bool dust_sent_elsewhere = (dust_policy.addr_for_dust.m_view_public_key != change_dts.addr.m_view_public_key
                              || dust_policy.addr_for_dust.m_spend_public_key != change_dts.addr.m_spend_public_key);

  if (dust_policy.add_to_fee || dust_sent_elsewhere) change_dts.amount -= dust;

  ptx.key_images = key_images;
  ptx.fee = (dust_policy.add_to_fee ? fee+dust : fee);
  ptx.dust = ((dust_policy.add_to_fee || dust_sent_elsewhere) ? dust : 0);
  ptx.dust_added_to_fee = dust_policy.add_to_fee;
  ptx.tx = tx;
  ptx.change_dts = change_dts;
  ptx.change_token_dts = change_token_dts;
  ptx.selected_transfers = selected_transfers;
  ptx.tx_key = tx_key;
  ptx.additional_tx_keys = additional_tx_keys;
  ptx.dests = dsts;
  ptx.construction_data.sources = sources;
  ptx.construction_data.change_dts = change_dts;
  ptx.construction_data.splitted_dsts = splitted_dsts;
  ptx.construction_data.selected_transfers = selected_transfers;
  ptx.construction_data.extra = tx.extra;
  ptx.construction_data.unlock_time = unlock_time;
  ptx.construction_data.use_rct = false;
  ptx.construction_data.dests = dsts;
  // record which subaddress indices are being used as inputs
  ptx.construction_data.subaddr_account = subaddr_account;
  ptx.construction_data.subaddr_indices.clear();
  for (size_t idx: selected_transfers)
    ptx.construction_data.subaddr_indices.insert(m_transfers[idx].m_subaddr_index.minor);
  LOG_PRINT_L2("transfer_selected done");
}


static uint32_t get_count_above(const std::vector<wallet::transfer_details> &transfers, const std::vector<size_t> &indices, uint64_t threshold)
{
  uint32_t count = 0;
  for (size_t idx: indices)
    if (transfers[idx].amount() >= threshold)
      ++count;
  return count;
}

bool wallet::light_wallet_login(bool &new_address)
{
  MDEBUG("Light wallet login request");
  m_light_wallet_connected = false;
  cryptonote::COMMAND_RPC_LOGIN::request request;
  cryptonote::COMMAND_RPC_LOGIN::response response;
  request.address = get_account().get_public_address_str(m_nettype);
  request.view_key = string_tools::pod_to_hex(get_account().get_keys().m_view_secret_key);
  // Always create account if it doesn't exist.
  request.create_account = true;
  m_daemon_rpc_mutex.lock();
  bool connected = epee::net_utils::invoke_http_json("/login", request, response, m_http_client, rpc_timeout, "POST");
  m_daemon_rpc_mutex.unlock();
  // MyMonero doesn't send any status message. OpenMonero does.
  m_light_wallet_connected  = connected && (response.status.empty() || response.status == "success");
  new_address = response.new_address;
  MDEBUG("Status: " << response.status);
  MDEBUG("Reason: " << response.reason);
  MDEBUG("New wallet: " << response.new_address);
  if(m_light_wallet_connected)
  {
    // Clear old data on successful login.
    // m_transfers.clear();
    // m_payments.clear();
    // m_unconfirmed_payments.clear();
  }
  return m_light_wallet_connected;
}

bool wallet::light_wallet_import_wallet_request(cryptonote::COMMAND_RPC_IMPORT_WALLET_REQUEST::response &response)
{
  MDEBUG("Light wallet import wallet request");
  cryptonote::COMMAND_RPC_IMPORT_WALLET_REQUEST::request oreq;
  oreq.address = get_account().get_public_address_str(m_nettype);
  oreq.view_key = string_tools::pod_to_hex(get_account().get_keys().m_view_secret_key);
  m_daemon_rpc_mutex.lock();
  bool r = epee::net_utils::invoke_http_json("/import_wallet_request", oreq, response, m_http_client, rpc_timeout, "POST");
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "import_wallet_request");


  return true;
}

void wallet::light_wallet_get_unspent_outs()
{
  MDEBUG("Getting unspent outs");

  cryptonote::COMMAND_RPC_GET_UNSPENT_OUTS::request oreq;
  cryptonote::COMMAND_RPC_GET_UNSPENT_OUTS::response ores;

  oreq.amount = "0";
  oreq.address = get_account().get_public_address_str(m_nettype);
  oreq.view_key = string_tools::pod_to_hex(get_account().get_keys().m_view_secret_key);
  // openMonero specific
  oreq.dust_threshold = boost::lexical_cast<std::string>(::config::DEFAULT_DUST_THRESHOLD);
  // below are required by openMonero api - but are not used.
  oreq.mixin = 0;
  oreq.use_dust = true;


  m_daemon_rpc_mutex.lock();
  bool r = epee::net_utils::invoke_http_json("/get_unspent_outs", oreq, ores, m_http_client, rpc_timeout, "POST");
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_unspent_outs");
  THROW_WALLET_EXCEPTION_IF(ores.status == "error", error::wallet_internal_error, ores.reason);

  m_light_wallet_per_kb_fee = ores.per_kb_fee;

  std::unordered_map<crypto::hash,bool> transfers_txs;
  for(const auto &t: m_transfers)
    transfers_txs.emplace(t.m_txid,t.m_spent);

  MDEBUG("FOUND " << ores.outputs.size() <<" outputs");

  // return if no outputs found
  if(ores.outputs.empty())
    return;

  // Clear old outputs
  m_transfers.clear();

  for (const auto &o: ores.outputs) {
    bool spent = false;
    bool add_transfer = true;
    crypto::key_image unspent_key_image;
    crypto::public_key tx_public_key = AUTO_VAL_INIT(tx_public_key);
    THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, o.tx_pub_key), error::wallet_internal_error, "Invalid tx_pub_key field");
    string_tools::hex_to_pod(o.tx_pub_key, tx_public_key);

    for (const std::string &ski: o.spend_key_images) {
      spent = false;

      // Check if key image is ours
      THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, ski), error::wallet_internal_error, "Invalid key image");
      string_tools::hex_to_pod(ski, unspent_key_image);
      if(light_wallet_key_image_is_ours(unspent_key_image, tx_public_key, o.index)){
        MTRACE("Output " << o.public_key << " is spent. Key image: " <<  ski);
        spent = true;
        break;
      } {
        MTRACE("Unspent output found. " << o.public_key);
      }
    }

    // Check if tx already exists in m_transfers.
    crypto::hash txid;
    crypto::public_key tx_pub_key;
    crypto::public_key public_key;
    THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, o.tx_hash), error::wallet_internal_error, "Invalid tx_hash field");
    THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, o.public_key), error::wallet_internal_error, "Invalid public_key field");
    THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, o.tx_pub_key), error::wallet_internal_error, "Invalid tx_pub_key field");
    string_tools::hex_to_pod(o.tx_hash, txid);
    string_tools::hex_to_pod(o.public_key, public_key);
    string_tools::hex_to_pod(o.tx_pub_key, tx_pub_key);

    for(auto &t: m_transfers){
      if(t.get_public_key() == public_key) {
        t.m_spent = spent;
        add_transfer = false;
        break;
      }
    }

    if(!add_transfer)
      continue;

    m_transfers.push_back(boost::value_initialized<transfer_details>());
    transfer_details& td = m_transfers.back();

    td.m_block_height = o.height;
    td.m_global_output_index = o.global_index;
    td.m_txid = txid;

    // Add to extra
    add_tx_pub_key_to_extra(td.m_tx, tx_pub_key);

    td.m_key_image = unspent_key_image;
    td.m_key_image_known = !m_watch_only;
    td.m_key_image_partial = m_multisig;
    td.m_amount = o.amount;
    td.m_pk_index = 0;
    td.m_internal_output_index = o.index;
    td.m_spent = spent;

    tx_out txout;
    txout.target = txout_to_key(public_key);
    txout.amount = td.m_amount;

    td.m_tx.vout.resize(td.m_internal_output_index + 1);
    td.m_tx.vout[td.m_internal_output_index] = txout;

    // Add unlock time and coinbase bool got from get_address_txs api call
    std::unordered_map<crypto::hash,address_tx>::const_iterator found = m_light_wallet_address_txs.find(txid);
    THROW_WALLET_EXCEPTION_IF(found == m_light_wallet_address_txs.end(), error::wallet_internal_error, "Lightwallet: tx not found in m_light_wallet_address_txs");
    bool miner_tx = found->second.m_coinbase;
    td.m_tx.unlock_time = found->second.m_unlock_time;

    if (!o.rct.empty())
    {
      // Coinbase tx's
      if(miner_tx)
      {
        td.m_mask = rct::identity();
      }
      else
      {
        // rct txs
        // decrypt rct mask, calculate commit hash and compare against blockchain commit hash
        rct::key rct_commit;
        light_wallet_parse_rct_str(o.rct, tx_pub_key, td.m_internal_output_index, td.m_mask, rct_commit, true);
        bool valid_commit = (rct_commit == rct::commit(td.amount(), td.m_mask));
        if(!valid_commit)
        {
          MDEBUG("output index: " << o.global_index);
          MDEBUG("mask: " + string_tools::pod_to_hex(td.m_mask));
          MDEBUG("calculated commit: " + string_tools::pod_to_hex(rct::commit(td.amount(), td.m_mask)));
          MDEBUG("expected commit: " + string_tools::pod_to_hex(rct_commit));
          MDEBUG("amount: " << td.amount());
        }
        THROW_WALLET_EXCEPTION_IF(!valid_commit, error::wallet_internal_error, "Lightwallet: rct commit hash mismatch!");
      }
      td.m_rct = true;
    }
    else
    {
      td.m_mask = rct::identity();
      td.m_rct = false;
    }
    if(!spent)
      set_unspent(m_transfers.size()-1);
    m_key_images[td.m_key_image] = m_transfers.size()-1;
    m_pub_keys[td.get_public_key()] = m_transfers.size()-1;
  }
}

bool wallet::light_wallet_get_address_info(cryptonote::COMMAND_RPC_GET_ADDRESS_INFO::response &response)
{
  MTRACE(__FUNCTION__);

  cryptonote::COMMAND_RPC_GET_ADDRESS_INFO::request request;

  request.address = get_account().get_public_address_str(m_nettype);
  request.view_key = string_tools::pod_to_hex(get_account().get_keys().m_view_secret_key);
  m_daemon_rpc_mutex.lock();
  bool r = epee::net_utils::invoke_http_json("/get_address_info", request, response, m_http_client, rpc_timeout, "POST");
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_address_info");
  // TODO: Validate result
  return true;
}

void wallet::light_wallet_get_address_txs()
{
  MDEBUG("Refreshing light wallet");

  cryptonote::COMMAND_RPC_GET_ADDRESS_TXS::request ireq;
  cryptonote::COMMAND_RPC_GET_ADDRESS_TXS::response ires;

  ireq.address = get_account().get_public_address_str(m_nettype);
  ireq.view_key = string_tools::pod_to_hex(get_account().get_keys().m_view_secret_key);
  m_daemon_rpc_mutex.lock();
  bool r = epee::net_utils::invoke_http_json("/get_address_txs", ireq, ires, m_http_client, rpc_timeout, "POST");
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_address_txs");
  //OpenMonero sends status=success, Mymonero doesn't.
  THROW_WALLET_EXCEPTION_IF((!ires.status.empty() && ires.status != "success"), error::no_connection_to_daemon, "get_address_txs");


  // Abort if no transactions
  if(ires.transactions.empty())
    return;

  // Create searchable vectors
  std::vector<crypto::hash> payments_txs;
  for(const auto &p: m_payments)
    payments_txs.push_back(p.second.m_tx_hash);
  std::vector<crypto::hash> unconfirmed_payments_txs;
  for(const auto &up: m_unconfirmed_payments)
    unconfirmed_payments_txs.push_back(up.second.m_pd.m_tx_hash);

  // for balance calculation
  uint64_t wallet_total_sent = 0;
  uint64_t wallet_total_unlocked_sent = 0;
  uint64_t wallet_total_token_sent = 0;
  // txs in pool
  std::vector<crypto::hash> pool_txs;

  for (const auto &t: ires.transactions) {
    const uint64_t total_received = t.total_received;
    const uint64_t token_total_received = t.total_received;
    uint64_t total_sent = t.total_sent;
    uint64_t token_total_sent = t.token_total_sent;

    // Check key images - subtract fake outputs from total_sent
    for(const auto &so: t.spent_outputs)
    {
      crypto::public_key tx_public_key;
      crypto::key_image key_image;
      THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, so.tx_pub_key), error::wallet_internal_error, "Invalid tx_pub_key field");
      THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, so.key_image), error::wallet_internal_error, "Invalid key_image field");
      string_tools::hex_to_pod(so.tx_pub_key, tx_public_key);
      string_tools::hex_to_pod(so.key_image, key_image);

      if(!light_wallet_key_image_is_ours(key_image, tx_public_key, so.out_index)) {
        THROW_WALLET_EXCEPTION_IF(so.amount > t.total_sent, error::wallet_internal_error, "Lightwallet: total sent is negative!");
        total_sent -= so.amount;
        THROW_WALLET_EXCEPTION_IF(so.token_amount > t.token_total_sent, error::wallet_internal_error, "Lightwallet: total sent is negative!");
        token_total_sent -= so.token_amount;
      }
    }

    // Do not add tx if empty.
    if ((total_sent == 0 && total_received == 0) && (token_total_sent == 0 && token_total_received == 0))
      continue;

    crypto::hash payment_id = null_hash;
    crypto::hash tx_hash;

    THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, t.payment_id), error::wallet_internal_error, "Invalid payment_id field");
    THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, t.hash), error::wallet_internal_error, "Invalid hash field");
    string_tools::hex_to_pod(t.payment_id, payment_id);
    string_tools::hex_to_pod(t.hash, tx_hash);

    // lightwallet specific info
    bool incoming = (total_received > total_sent) || (token_total_received > token_total_sent);
    address_tx address_tx = AUTO_VAL_INIT(address_tx);
    address_tx.m_tx_hash = tx_hash;
    address_tx.m_incoming = incoming;
    address_tx.m_amount  =  incoming ? total_received - total_sent : total_sent - total_received;
    address_tx.m_token_amount  =  incoming ? token_total_received - token_total_sent : token_total_sent - token_total_received;
    address_tx.m_fee = 0;                 // TODO
    address_tx.m_block_height = t.height;
    address_tx.m_unlock_time  = t.unlock_time;
    address_tx.m_timestamp = t.timestamp;
    address_tx.m_coinbase  = t.coinbase;
    address_tx.m_mempool  = t.mempool;
    m_light_wallet_address_txs.emplace(tx_hash,address_tx);

    // populate data needed for history (m_payments, m_unconfirmed_payments, m_confirmed_txs)
    // INCOMING transfers
    if(total_received > total_sent) {
      payment_details payment = AUTO_VAL_INIT(payment);
      payment.m_tx_hash = tx_hash;
      payment.m_amount       = total_received - total_sent;
      payment.m_token_amount       = token_total_received - token_total_sent;
      payment.m_token_transaction = t.token_transaction;
      payment.m_fee          = 0;         // TODO
      payment.m_block_height = t.height;
      payment.m_unlock_time  = t.unlock_time;
      payment.m_timestamp = t.timestamp;

      if (t.mempool) {
        if (std::find(unconfirmed_payments_txs.begin(), unconfirmed_payments_txs.end(), tx_hash) == unconfirmed_payments_txs.end()) {
          pool_txs.push_back(tx_hash);
          // assume false as we don't get that info from the light wallet server
          crypto::hash payment_id;
          THROW_WALLET_EXCEPTION_IF(!epee::string_tools::hex_to_pod(t.payment_id, payment_id),
              error::wallet_internal_error, "Failed to parse payment id");
          emplace_or_replace(m_unconfirmed_payments, payment_id, pool_payment_details{payment, false});
          if (0 != m_callback) {
            if (payment.m_token_transaction)
              m_callback->on_lw_unconfirmed_token_received(t.height, payment.m_tx_hash, payment.m_token_amount);
            else
              m_callback->on_lw_unconfirmed_money_received(t.height, payment.m_tx_hash, payment.m_amount);

          }
        }
      } else {
        if (std::find(payments_txs.begin(), payments_txs.end(), tx_hash) == payments_txs.end()) {
          m_payments.emplace(tx_hash, payment);
          if (0 != m_callback) {
            if (payment.m_token_transaction)
              m_callback->on_lw_token_received(t.height, payment.m_tx_hash, payment.m_token_amount);
            else
              m_callback->on_lw_money_received(t.height, payment.m_tx_hash, payment.m_amount);
          }
        }
      }
    // Outgoing transfers
    } else {
      uint64_t amount_sent = total_sent - total_received;
      uint64_t token_amount_sent = token_total_sent - token_total_received;
      cryptonote::transaction dummy_tx; // not used by light wallet
      // increase wallet total sent
      wallet_total_sent += total_sent;
      wallet_total_token_sent += token_total_sent;
      if (t.mempool)
      {
        // Handled by add_unconfirmed_tx in commit_tx
        // If sent from another wallet instance we need to add it
        if(m_unconfirmed_txs.find(tx_hash) == m_unconfirmed_txs.end())
        {
          unconfirmed_transfer_details utd = AUTO_VAL_INIT(utd);
          utd.m_amount_in = amount_sent;
          utd.m_amount_out = amount_sent;
          utd.m_token_amount_in = token_amount_sent;
          utd.m_token_amount_out = token_amount_sent;
          utd.m_change = 0;
          utd.m_token_change = 0;
          utd.m_payment_id = payment_id;
          utd.m_timestamp = t.timestamp;
          utd.m_state = wallet::unconfirmed_transfer_details::pending;
          m_unconfirmed_txs.emplace(tx_hash,utd);
        }
      }
      else
      {
        // Only add if new
        auto confirmed_tx = m_confirmed_txs.find(tx_hash);
        if(confirmed_tx == m_confirmed_txs.end()) {
          // tx is added to m_unconfirmed_txs - move to confirmed
          if(m_unconfirmed_txs.find(tx_hash) != m_unconfirmed_txs.end())
          {
            process_unconfirmed(tx_hash, dummy_tx, t.height);
          }
          // Tx sent by another wallet instance
          else
          {
            confirmed_transfer_details ctd;
            ctd.m_amount_in = amount_sent;
            ctd.m_amount_out = amount_sent;
            ctd.m_token_amount_in = token_amount_sent;
            ctd.m_token_amount_out = token_amount_sent;
            ctd.m_change = 0;
            ctd.m_payment_id = payment_id;
            ctd.m_block_height = t.height;
            ctd.m_timestamp = t.timestamp;
            m_confirmed_txs.emplace(tx_hash,ctd);
          }
          if (0 != m_callback)
          {
            if (t.token_transaction)
              m_callback->on_lw_token_spent(t.height, tx_hash, token_amount_sent);
            else
              m_callback->on_lw_money_spent(t.height, tx_hash, amount_sent);
          }
        }
        // If not new - check the amount and update if necessary.
        // when sending a tx to same wallet the receiving amount has to be credited
        else
        {
          if(confirmed_tx->second.m_amount_in != amount_sent || confirmed_tx->second.m_amount_out != amount_sent)
          {
            MDEBUG("Adjusting amount sent/received for tx: <" + t.hash + ">. Is tx sent to own wallet? " << print_money(amount_sent) << " != " << print_money(confirmed_tx->second.m_amount_in));
            confirmed_tx->second.m_amount_in = amount_sent;
            confirmed_tx->second.m_amount_out = amount_sent;
            confirmed_tx->second.m_change = 0;
          }

          if(confirmed_tx->second.m_token_amount_in != token_amount_sent || confirmed_tx->second.m_token_amount_out != token_amount_sent)
          {
            MDEBUG("Adjusting token amount sent/received for tx: <" + t.hash + ">. Is tx sent to own wallet? " << print_money(token_amount_sent) << " != " << print_money(confirmed_tx->second.m_token_amount_in));
            confirmed_tx->second.m_token_amount_in = token_amount_sent;
            confirmed_tx->second.m_token_amount_out = token_amount_sent;
            confirmed_tx->second.m_token_change = 0;
          }
        }
      }
    }
  }
  // TODO: purge old unconfirmed_txs
  remove_obsolete_pool_txs(pool_txs);

  // Calculate wallet balance
  m_light_wallet_balance = ires.total_received-wallet_total_sent;
  m_light_wallet_token_balance = ires.token_total_received-wallet_total_token_sent;

  // MyMonero doesn't send unlocked balance
  if(ires.total_received_unlocked > 0)
    m_light_wallet_unlocked_balance = ires.total_received_unlocked-wallet_total_sent;
  else
    m_light_wallet_unlocked_balance = m_light_wallet_balance;

  if(ires.token_total_received_unlocked > 0)
    m_light_wallet_unlocked_token_balance = ires.token_total_received_unlocked-wallet_total_token_sent;
  else
    m_light_wallet_unlocked_token_balance = m_light_wallet_token_balance;
}

bool wallet::light_wallet_parse_rct_str(const std::string& rct_string, const crypto::public_key& tx_pub_key, uint64_t internal_output_index, rct::key& decrypted_mask, rct::key& rct_commit, bool decrypt) const
{
  // rct string is empty if output is non RCT
  if (rct_string.empty())
    return false;
  // rct_string is a string with length 64+64+64 (<rct commit> + <encrypted mask> + <rct amount>)
  rct::key encrypted_mask;
  std::string rct_commit_str = rct_string.substr(0,64);
  std::string encrypted_mask_str = rct_string.substr(64,64);
  THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, rct_commit_str), error::wallet_internal_error, "Invalid rct commit hash: " + rct_commit_str);
  THROW_WALLET_EXCEPTION_IF(string_tools::validate_hex(64, encrypted_mask_str), error::wallet_internal_error, "Invalid rct mask: " + encrypted_mask_str);
  string_tools::hex_to_pod(rct_commit_str, rct_commit);
  string_tools::hex_to_pod(encrypted_mask_str, encrypted_mask);
  if (decrypt) {
    // Decrypt the mask
    crypto::key_derivation derivation;
    bool r = generate_key_derivation(tx_pub_key, get_account().get_keys().m_view_secret_key, derivation);
    THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key derivation");
    crypto::secret_key scalar;
    crypto::derivation_to_scalar(derivation, internal_output_index, scalar);
    sc_sub(decrypted_mask.bytes,encrypted_mask.bytes,rct::hash_to_scalar(rct::sk2rct(scalar)).bytes);
  }
  return true;
}

bool wallet::light_wallet_key_image_is_ours(const crypto::key_image& key_image, const crypto::public_key& tx_public_key, uint64_t out_index)
{
  // Lookup key image from cache
  std::map<uint64_t, crypto::key_image> index_keyimage_map;
  std::unordered_map<crypto::public_key, std::map<uint64_t, crypto::key_image> >::const_iterator found_pub_key = m_key_image_cache.find(tx_public_key);
  if(found_pub_key != m_key_image_cache.end()) {
    // pub key found. key image for index cached?
    index_keyimage_map = found_pub_key->second;
    std::map<uint64_t,crypto::key_image>::const_iterator index_found = index_keyimage_map.find(out_index);
    if(index_found != index_keyimage_map.end())
      return key_image == index_found->second;
  }

  // Not in cache - calculate key image
  crypto::key_image calculated_key_image;
  cryptonote::keypair in_ephemeral;

  // Subaddresses aren't supported in mymonero/openmonero yet. Roll out the original scheme:
  //   compute D = a*R
  //   compute P = Hs(D || i)*G + B
  //   compute x = Hs(D || i) + b      (and check if P==x*G)
  //   compute I = x*Hp(P)
  const account_keys& ack = get_account().get_keys();
  crypto::key_derivation derivation;
  bool r = crypto::generate_key_derivation(tx_public_key, ack.m_view_secret_key, derivation);
  CHECK_AND_ASSERT_MES(r, false, "failed to generate_key_derivation(" << tx_public_key << ", " << ack.m_view_secret_key << ")");

  r = crypto::derive_public_key(derivation, out_index, ack.m_account_address.m_spend_public_key, in_ephemeral.pub);
  CHECK_AND_ASSERT_MES(r, false, "failed to derive_public_key (" << derivation << ", " << out_index << ", " << ack.m_account_address.m_spend_public_key << ")");

  crypto::derive_secret_key(derivation, out_index, ack.m_spend_secret_key, in_ephemeral.sec);
  crypto::public_key out_pkey_test;
  r = crypto::secret_key_to_public_key(in_ephemeral.sec, out_pkey_test);
  CHECK_AND_ASSERT_MES(r, false, "failed to secret_key_to_public_key(" << in_ephemeral.sec << ")");
  CHECK_AND_ASSERT_MES(in_ephemeral.pub == out_pkey_test, false, "derived secret key doesn't match derived public key");

  crypto::generate_key_image(in_ephemeral.pub, in_ephemeral.sec, calculated_key_image);

  index_keyimage_map.emplace(out_index, calculated_key_image);
  m_key_image_cache.emplace(tx_public_key, index_keyimage_map);
  return key_image == calculated_key_image;
}

// Another implementation of transaction creation that is hopefully better
// While there is anything left to pay, it goes through random outputs and tries
// to fill the next destination/amount. If it fully fills it, it will use the
// remainder to try to fill the next one as well.
// The tx size if roughly estimated as a linear function of only inputs, and a
// new tx will be created when that size goes above a given fraction of the
// max tx size. At that point, more outputs may be added if the fee cannot be
// satisfied.
// If the next output in the next tx would go to the same destination (ie, we
// cut off at a tx boundary in the middle of paying a given destination), the
// fee will be carved out of the current input if possible, to avoid having to
// add another output just for the fee and getting change.
// This system allows for sending (almost) the entire balance, since it does
// not generate spurious change in all txes, thus decreasing the instantaneous
// usable balance.
std::vector<wallet::pending_tx> wallet::create_transactions_2(std::vector<cryptonote::tx_destination_entry> dsts, const size_t fake_outs_count, const uint64_t unlock_time, uint32_t priority, const std::vector<uint8_t>& extra, uint32_t subaddr_account, std::set<uint32_t> subaddr_indices, bool trusted_daemon)
{
  //ensure device is let in NONE mode in any case
  hw::device &hwdev = m_account.get_device();
  boost::unique_lock<hw::device> hwdev_lock (hwdev);
  hw::reset_mode rst(hwdev);

  if(m_light_wallet) {
    // Populate m_transfers
    light_wallet_get_unspent_outs();
  }
  std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_transfers_indices_per_subaddr;
  std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_dust_indices_per_subaddr;
  uint64_t needed_money;
  struct TX {
    std::vector<size_t> selected_transfers;
    std::vector<cryptonote::tx_destination_entry> dsts;
    cryptonote::transaction tx;
    pending_tx ptx;
    size_t bytes;
    std::vector<std::vector<tools::wallet::get_outs_entry>> outs;

    void add(const account_public_address &addr, bool is_subaddress, uint64_t amount, unsigned int original_output_index, bool merge_destinations) {
      if (merge_destinations)
      {
        std::vector<cryptonote::tx_destination_entry>::iterator i;
        i = std::find_if(dsts.begin(), dsts.end(), [&](const cryptonote::tx_destination_entry &d) { return !memcmp (&d.addr, &addr, sizeof(addr)); });
        if (i == dsts.end())
        {
          dsts.push_back(tx_destination_entry(0,addr,is_subaddress));
          i = dsts.end() - 1;
        }
        i->amount += amount;
      }
      else
      {
        THROW_WALLET_EXCEPTION_IF(original_output_index > dsts.size(), error::wallet_internal_error,
            std::string("original_output_index too large: ") + std::to_string(original_output_index) + " > " + std::to_string(dsts.size()));
        if (original_output_index == dsts.size())
          dsts.push_back(tx_destination_entry(0,addr,is_subaddress));
        THROW_WALLET_EXCEPTION_IF(memcmp(&dsts[original_output_index].addr, &addr, sizeof(addr)), error::wallet_internal_error, "Mismatched destination address");
        dsts[original_output_index].amount += amount;
      }
    }
  };

  uint64_t upper_transaction_size_limit = get_upper_transaction_size_limit();
  const uint64_t fee_per_kb  = get_per_kb_fee();
  const uint64_t fee_multiplier = get_fee_multiplier(priority, get_fee_algorithm());

  // throw if attempting a transaction with no destinations
  THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);

  // calculate total amount being sent to all destinations
  // throw if total amount overflows uint64_t
  needed_money = 0;
  for(auto& dt: dsts)
  {
    THROW_WALLET_EXCEPTION_IF(0 == dt.amount, error::zero_destination);
    needed_money += dt.amount;
    LOG_PRINT_L2("transfer: adding " << print_money(dt.amount) << ", for a total of " << print_money (needed_money));
    THROW_WALLET_EXCEPTION_IF(needed_money < dt.amount, error::tx_sum_overflow, dsts, 0, m_nettype);
  }

  // throw if attempting a transaction with no money
  THROW_WALLET_EXCEPTION_IF(needed_money == 0, error::zero_destination);

  std::map<uint32_t, uint64_t> unlocked_balance_per_subaddr = unlocked_balance_per_subaddress(subaddr_account);
  std::map<uint32_t, uint64_t> balance_per_subaddr = balance_per_subaddress(subaddr_account);

  if (subaddr_indices.empty()) // "index=<N1>[,<N2>,...]" wasn't specified -> use all the indices with non-zero unlocked balance
  {
    for (const auto& i : balance_per_subaddr)
      subaddr_indices.insert(i.first);
  }

  // early out if we know we can't make it anyway
  // we could also check for being within FEE_PER_KB, but if the fee calculation
  // ever changes, this might be missed, so let this go through
  uint64_t balance_subtotal = 0;
  uint64_t unlocked_balance_subtotal = 0;
  for (uint32_t index_minor : subaddr_indices)
  {
    balance_subtotal += balance_per_subaddr[index_minor];
    unlocked_balance_subtotal += unlocked_balance_per_subaddr[index_minor];
  }
  THROW_WALLET_EXCEPTION_IF(needed_money > balance_subtotal, error::not_enough_cash,
    balance_subtotal, needed_money, 0);
  // first check overall balance is enough, then unlocked one, so we throw distinct exceptions
  THROW_WALLET_EXCEPTION_IF(needed_money > unlocked_balance_subtotal, error::not_enough_unlocked_cash,
      unlocked_balance_subtotal, needed_money, 0);

  for (uint32_t i : subaddr_indices)
    LOG_PRINT_L2("Candidate subaddress index for spending: " << i);

  // gather all dust and non-dust outputs belonging to specified subaddresses
  size_t num_nondust_outputs = 0;
  size_t num_dust_outputs = 0;
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    const transfer_details& td = m_transfers[i];
    if (!td.m_spent && !td.m_key_image_partial && is_transfer_unlocked(td) && td.m_subaddr_index.major == subaddr_account && subaddr_indices.count(td.m_subaddr_index.minor) == 1)
    {
      const uint32_t index_minor = td.m_subaddr_index.minor;
      auto find_predicate = [&index_minor](const std::pair<uint32_t, std::vector<size_t>>& x) { return x.first == index_minor; };
      if ((td.is_rct()) || is_valid_decomposed_amount(td.amount()))
      {
        auto found = std::find_if(unused_transfers_indices_per_subaddr.begin(), unused_transfers_indices_per_subaddr.end(), find_predicate);
        if (found == unused_transfers_indices_per_subaddr.end())
        {
          unused_transfers_indices_per_subaddr.push_back({index_minor, {i}});
        }
        else
        {
          found->second.push_back(i);
        }
        ++num_nondust_outputs;
      }
      else
      {
        auto found = std::find_if(unused_dust_indices_per_subaddr.begin(), unused_dust_indices_per_subaddr.end(), find_predicate);
        if (found == unused_dust_indices_per_subaddr.end())
        {
          unused_dust_indices_per_subaddr.push_back({index_minor, {i}});
        }
        else
        {
          found->second.push_back(i);
        }
        ++num_dust_outputs;
      }
    }
  }

  // shuffle & sort output indices
  {
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(unused_transfers_indices_per_subaddr.begin(), unused_transfers_indices_per_subaddr.end(), g);
    std::shuffle(unused_dust_indices_per_subaddr.begin(), unused_dust_indices_per_subaddr.end(), g);
    auto sort_predicate = [&unlocked_balance_per_subaddr] (const std::pair<uint32_t, std::vector<size_t>>& x, const std::pair<uint32_t, std::vector<size_t>>& y)
    {
      return unlocked_balance_per_subaddr[x.first] > unlocked_balance_per_subaddr[y.first];
    };
    std::sort(unused_transfers_indices_per_subaddr.begin(), unused_transfers_indices_per_subaddr.end(), sort_predicate);
    std::sort(unused_dust_indices_per_subaddr.begin(), unused_dust_indices_per_subaddr.end(), sort_predicate);
  }

  LOG_PRINT_L2("Starting with " << num_nondust_outputs << " non-dust outputs and " << num_dust_outputs << " dust outputs");

  if (unused_dust_indices_per_subaddr.empty() && unused_transfers_indices_per_subaddr.empty())
    return std::vector<wallet::pending_tx>();

  // if empty, put dummy entry so that the front can be referenced later in the loop
  if (unused_dust_indices_per_subaddr.empty())
    unused_dust_indices_per_subaddr.push_back({});
  if (unused_transfers_indices_per_subaddr.empty())
    unused_transfers_indices_per_subaddr.push_back({});


  std::vector<TX> txes;
  txes.push_back(TX()); // start with an empty tx

  bool adding_fee = false; // true if new outputs go towards fee, rather than destinations
  uint64_t needed_fee = 0;
  uint64_t available_for_fee = 0;
  uint64_t accumulated_fee = 0, accumulated_outputs = 0, accumulated_change = 0;

  std::vector<std::vector<tools::wallet::get_outs_entry>> outs;


  // while:
  // - we have something to send
  // - or we need to gather more fee
  // - or we have just one input in that tx, which is rct (to try and make all/most rct txes 2/2)
  unsigned int original_output_index = 0;
  std::vector<size_t>* unused_transfers_indices = &unused_transfers_indices_per_subaddr[0].second;
  std::vector<size_t>* unused_dust_indices      = &unused_dust_indices_per_subaddr[0].second;

  hwdev.set_mode(hw::device::TRANSACTION_CREATE_FAKE);
  while ((!dsts.empty() && dsts[0].amount > 0) || adding_fee) {
    TX &tx = txes.back();

    LOG_PRINT_L2("Start of loop with " << unused_transfers_indices->size() << " " << unused_dust_indices->size());
    LOG_PRINT_L2("unused_transfers_indices: " << (unused_transfers_indices->size() < 100 ? strjoin(*unused_transfers_indices, " ") : "too big to print"));
    LOG_PRINT_L2("unused_dust_indices: " << (unused_dust_indices->size() < 100 ? strjoin(*unused_dust_indices, " ") : "too big to print"));
    LOG_PRINT_L2("dsts size " << dsts.size() << ", first " << (dsts.empty() ? "-" : cryptonote::print_money(dsts[0].amount)));
    LOG_PRINT_L2("adding_fee " << adding_fee);

    // if we need to spend money and don't have any left, we fail
    if (unused_dust_indices->empty() && unused_transfers_indices->empty()) {
      LOG_PRINT_L2("No more outputs to choose from");
      THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_balance(subaddr_account), needed_money, accumulated_fee + needed_fee);
    }

    // get a random unspent output and use it to pay part (or all) of the current destination (and maybe next one, etc)
    // This could be more clever, but maybe at the cost of making probabilistic inferences easier
    size_t idx;
    if ((dsts.empty() || dsts[0].amount == 0) && !adding_fee) {
      // the "make rct txes 2/2" case - we pick a small value output to "clean up" the wallet too
      std::vector<size_t> indices;
      idx = pop_best_value(indices, tx.selected_transfers, true, tx_out_type::out_cash);

      // we might not want to add it if it's a large output and we don't have many left
      if (m_transfers[idx].amount() >= m_min_output_value) {
        if (get_count_above(m_transfers, *unused_transfers_indices, m_min_output_value) < m_min_output_count) {
          LOG_PRINT_L2("Second output was not strictly needed, and we're running out of outputs above " << print_money(m_min_output_value) << ", not adding");
          break;
        }
      }

      // since we're trying to add a second output which is not strictly needed,
      // we only add it if it's unrelated enough to the first one
      float relatedness = get_output_relatedness(m_transfers[idx], m_transfers[tx.selected_transfers.front()]);
      if (relatedness > SECOND_OUTPUT_RELATEDNESS_THRESHOLD)
      {
        LOG_PRINT_L2("Second output was not strictly needed, and relatedness " << relatedness << ", not adding");
        break;
      }
      pop_if_present(*unused_transfers_indices, idx);
      pop_if_present(*unused_dust_indices, idx);
    } else
      idx = pop_best_value(unused_transfers_indices->empty() ? *unused_dust_indices : *unused_transfers_indices, tx.selected_transfers, false, tx_out_type::out_cash);

    const transfer_details &td = m_transfers[idx];
    LOG_PRINT_L2("Picking output " << idx << ", amount " << print_money(td.amount()) << ", ki " << td.m_key_image);

    // add this output to the list to spend
    tx.selected_transfers.push_back(idx);
    uint64_t available_amount = td.amount();
    accumulated_outputs += available_amount;

    // clear any fake outs we'd already gathered, since we'll need a new set
    outs.clear();

    if (adding_fee)
    {
      LOG_PRINT_L2("We need more fee, adding it to fee");
      available_for_fee += available_amount;
    }
    else
    {
      while (!dsts.empty() && dsts[0].amount <= available_amount && estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size()) < TX_SIZE_TARGET(upper_transaction_size_limit))
      {
        // we can fully pay that destination
        LOG_PRINT_L2("We can fully pay " << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
          " for " << print_money(dsts[0].amount));
        tx.add(dsts[0].addr, dsts[0].is_subaddress, dsts[0].amount, original_output_index, m_merge_destinations);
        available_amount -= dsts[0].amount;
        dsts[0].amount = 0;
        pop_index(dsts, 0);
        ++original_output_index;
      }

      if (available_amount > 0 && !dsts.empty() && estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size()) < TX_SIZE_TARGET(upper_transaction_size_limit)) {
        // we can partially fill that destination
        LOG_PRINT_L2("We can partially pay " << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
          " for " << print_money(available_amount) << "/" << print_money(dsts[0].amount));
        tx.add(dsts[0].addr, dsts[0].is_subaddress, available_amount, original_output_index, m_merge_destinations);
        dsts[0].amount -= available_amount;
        available_amount = 0;
      }
    }

    // here, check if we need to sent tx and start a new one
    LOG_PRINT_L2("Considering whether to create a tx now, " << tx.selected_transfers.size() << " inputs, tx limit "
      << upper_transaction_size_limit);
    bool try_tx = false;

    if (adding_fee)
    {
      /* might not actually be enough if adding this output bumps size to next kB, but we need to try */
      try_tx = available_for_fee >= needed_fee;
    }
    else
    {
      const size_t estimated_rct_tx_size = estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size());
      try_tx = dsts.empty() || (estimated_rct_tx_size >= TX_SIZE_TARGET(upper_transaction_size_limit));
    }


    if (try_tx) {
      cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
      pending_tx test_ptx = AUTO_VAL_INIT(test_ptx);

      const size_t estimated_tx_size = estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size());
      needed_fee = calculate_fee(fee_per_kb, estimated_tx_size, fee_multiplier);

      uint64_t inputs = 0, outputs = needed_fee;
      for (size_t idx: tx.selected_transfers) inputs += m_transfers[idx].amount();
      for (const auto &o: tx.dsts) outputs += o.amount;

      if (inputs < outputs)
      {
        LOG_PRINT_L2("We don't have enough for the basic fee, switching to adding_fee");
        adding_fee = true;
        goto skip_tx;
      }

      LOG_PRINT_L2("Trying to create a tx now, with " << tx.dsts.size() << " outputs and " <<
        tx.selected_transfers.size() << " inputs");
      transfer_selected(tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_fee, extra,
          detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);
      auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
      needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
      available_for_fee = test_ptx.fee + test_ptx.change_dts.amount + (!test_ptx.dust_added_to_fee ? test_ptx.dust : 0);
      LOG_PRINT_L2("Made a " << get_size_string(txBlob) << " tx, with " << print_money(available_for_fee) << " available for fee (" <<
        print_money(needed_fee) << " needed)");

      if (needed_fee > available_for_fee && !dsts.empty() && dsts[0].amount > 0)
      {
        // we don't have enough for the fee, but we've only partially paid the current address,
        // so we can take the fee from the paid amount, since we'll have to make another tx anyway
        std::vector<cryptonote::tx_destination_entry>::iterator i;
        i = std::find_if(tx.dsts.begin(), tx.dsts.end(),
          [&](const cryptonote::tx_destination_entry &d) { return !memcmp (&d.addr, &dsts[0].addr, sizeof(dsts[0].addr)); });
        THROW_WALLET_EXCEPTION_IF(i == tx.dsts.end(), error::wallet_internal_error, "paid address not found in outputs");
        if (i->amount > needed_fee)
        {
          uint64_t new_paid_amount = i->amount /*+ test_ptx.fee*/ - needed_fee;
          LOG_PRINT_L2("Adjusting amount paid to " << get_account_address_as_str(m_nettype, i->is_subaddress, i->addr) << " from " <<
            print_money(i->amount) << " to " << print_money(new_paid_amount) << " to accommodate " <<
            print_money(needed_fee) << " fee");
          dsts[0].amount += i->amount - new_paid_amount;
          i->amount = new_paid_amount;
          test_ptx.fee = needed_fee;
          available_for_fee = needed_fee;
        }
      }

      if (needed_fee > available_for_fee)
      {
        LOG_PRINT_L2("We could not make a tx, switching to fee accumulation");

        adding_fee = true;
      }
      else
      {
        LOG_PRINT_L2("We made a tx, adjusting fee and saving it, we need " << print_money(needed_fee) << " and we have " << print_money(test_ptx.fee));
        while (needed_fee > test_ptx.fee) {

        transfer_selected(tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_fee, extra,
              detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);
          txBlob = t_serializable_object_to_blob(test_ptx.tx);
          needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
          LOG_PRINT_L2("Made an attempt at a  final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
            " fee  and " << print_money(test_ptx.change_dts.amount) << " change");
        }

        LOG_PRINT_L2("Made a final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
          " fee  and " << print_money(test_ptx.change_dts.amount) << " change");

        tx.tx = test_tx;
        tx.ptx = test_ptx;
        tx.bytes = txBlob.size();
        tx.outs = outs;
        accumulated_fee += test_ptx.fee;
        accumulated_change += test_ptx.change_dts.amount;
        adding_fee = false;
        if (!dsts.empty())
        {
          LOG_PRINT_L2("We have more to pay, starting another tx");
          txes.push_back(TX());
          original_output_index = 0;
        }
      }
    }

skip_tx:
    // if unused_*_indices is empty while unused_*_indices_per_subaddr has multiple elements, and if we still have something to pay,
    // pop front of unused_*_indices_per_subaddr and have unused_*_indices point to the front of unused_*_indices_per_subaddr
    if ((!dsts.empty() && dsts[0].amount > 0) || adding_fee)
    {
      if (unused_transfers_indices->empty() && unused_transfers_indices_per_subaddr.size() > 1)
      {
        unused_transfers_indices_per_subaddr.erase(unused_transfers_indices_per_subaddr.begin());
        unused_transfers_indices = &unused_transfers_indices_per_subaddr[0].second;
      }
      if (unused_dust_indices->empty() && unused_dust_indices_per_subaddr.size() > 1)
      {
        unused_dust_indices_per_subaddr.erase(unused_dust_indices_per_subaddr.begin());
        unused_dust_indices = &unused_dust_indices_per_subaddr[0].second;
      }
    }
  }

  if (adding_fee)
  {
    LOG_PRINT_L1("We ran out of outputs while trying to gather final fee");
    THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_balance(subaddr_account), needed_money, accumulated_fee + needed_fee);
  }

  LOG_PRINT_L1("Done creating " << txes.size() << " transactions, " << print_money(accumulated_fee) <<
    " total fee, " << print_money(accumulated_change) << " total change");

  hwdev.set_mode(hw::device::TRANSACTION_CREATE_REAL);
  for (std::vector<TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TX &tx = *i;
    cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
    pending_tx test_ptx = AUTO_VAL_INIT(test_ptx);

    transfer_selected(tx.dsts,
                        tx.selected_transfers,
                        fake_outs_count,
                        tx.outs,
                        unlock_time,
                        tx.ptx.fee,
                        extra,
                        detail::digit_split_strategy,
                        tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD),
                        test_tx,
                        test_ptx);

    auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
    tx.tx = test_tx;
    tx.ptx = test_ptx;
    tx.bytes = txBlob.size();
  }

  std::vector<wallet::pending_tx> ptx_vector;
  for (std::vector<TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TX &tx = *i;
    uint64_t tx_money = 0;
    for (size_t idx: tx.selected_transfers)
      tx_money += m_transfers[idx].amount();
    LOG_PRINT_L1("  Transaction " << (1+std::distance(txes.begin(), i)) << "/" << txes.size() <<
      ": " << get_size_string(tx.bytes) << ", sending " << print_money(tx_money) << " in " << tx.selected_transfers.size() <<
      " outputs to " << tx.dsts.size() << " destination(s), including " <<
      print_money(tx.ptx.fee) << " fee, " << print_money(tx.ptx.change_dts.amount) << " change");
    ptx_vector.push_back(tx.ptx);
  }

  // if we made it this far, we're OK to actually send the transactions
  return ptx_vector;
}


std::vector<wallet::pending_tx> wallet::create_transactions_token(std::vector<cryptonote::tx_destination_entry> dsts, const size_t fake_outs_count, const uint64_t unlock_time, uint32_t priority,
    const std::vector<uint8_t>& extra, uint32_t subaddr_account, std::set<uint32_t> subaddr_indices, bool trusted_daemon)
{
  //ensure device is let in NONE mode in any case

  hw::device &hwdev = m_account.get_device();
  boost::unique_lock<hw::device> hwdev_lock (hwdev);
  hw::reset_mode rst(hwdev);

  if(m_light_wallet) {
    // Populate m_transfers
    light_wallet_get_unspent_outs();
  }
  std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_token_transfers_indices_per_subaddr;
  std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_token_dust_indices_per_subaddr;
  std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_transfers_indices_per_subaddr; //for fee payment
  std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_dust_indices_per_subaddr; // for fee payment

  struct TOKEN_TX {
    std::vector<size_t> selected_transfers;
    std::vector<cryptonote::tx_destination_entry> dsts;
    cryptonote::transaction tx;
    pending_tx ptx = AUTO_VAL_INIT(ptx);
    size_t bytes = 0;
    std::vector<std::vector<tools::wallet::get_outs_entry>> outs;

    void add(const account_public_address &addr, bool is_subaddress, cryptonote::tx_out_type output_type, uint64_t amount, unsigned int original_output_index, bool merge_destinations) {
      if (merge_destinations)
      {
        std::vector<cryptonote::tx_destination_entry>::iterator i;
        i = std::find_if(dsts.begin(), dsts.end(), [&](const cryptonote::tx_destination_entry &d) { return !memcmp (&d.addr, &addr, sizeof(addr)); });
        if (i == dsts.end())
        {
          dsts.emplace_back(0, addr, is_subaddress, output_type);
          i = dsts.end() - 1;
        }
        if (output_type == cryptonote::tx_out_type::out_token)
        {
          THROW_WALLET_EXCEPTION_IF(!tools::is_whole_token_amount(amount), error::wallet_internal_error, "Token amount must be whole number.");
          i->token_amount += amount;
        }
        else
        {
          i->amount += amount;
        }
      }
      else
      {
        THROW_WALLET_EXCEPTION_IF(original_output_index > dsts.size(), error::wallet_internal_error,
            std::string("original_output_index too large: ") + std::to_string(original_output_index) + " > " + std::to_string(dsts.size()));
        if (original_output_index == dsts.size())
          dsts.emplace_back(0,addr,is_subaddress, output_type);
        THROW_WALLET_EXCEPTION_IF(memcmp(&dsts[original_output_index].addr, &addr, sizeof(addr)), error::wallet_internal_error, "Mismatched destination address");
        if (output_type == cryptonote::tx_out_type::out_token)
        {
          THROW_WALLET_EXCEPTION_IF(!tools::is_whole_token_amount(amount), error::wallet_internal_error, "Token amount must be whole number.");
          dsts[original_output_index].token_amount += amount;
        }
        else
        {
          dsts[original_output_index].amount += amount;
        }
      }
    }
  };

  uint64_t upper_transaction_size_limit = get_upper_transaction_size_limit();

  const uint64_t fee_per_kb  = get_per_kb_fee();
  const uint64_t fee_multiplier = get_fee_multiplier(priority, get_fee_algorithm());

  // throw if attempting a transaction with no destinations
  THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);

  // calculate total amount being sent to all destinations
  // throw if total amount overflows uint64_t
  uint64_t needed_money = 0; //this is for transaction fee
  uint64_t needed_tokens = 0;
  for(auto& dt: dsts)
  {
    THROW_WALLET_EXCEPTION_IF(0 == dt.token_amount, error::zero_destination);
    THROW_WALLET_EXCEPTION_IF(!tools::is_whole_token_amount(dt.token_amount), error::wallet_internal_error, "Token amount must be a while number.");
    needed_tokens += dt.token_amount;
    LOG_PRINT_L2("transfer: adding " << print_money(dt.token_amount) << " tokens, for a total of " << print_money (needed_tokens)) << " tokens";
    THROW_WALLET_EXCEPTION_IF(needed_tokens < dt.token_amount, error::tx_sum_overflow, dsts, 0, m_nettype);
  }

  // throw if attempting a transaction with no money
  THROW_WALLET_EXCEPTION_IF(needed_tokens == 0, error::zero_destination);

  std::map<uint32_t, uint64_t> unlocked_balance_per_subaddr = unlocked_balance_per_subaddress(subaddr_account);
  std::map<uint32_t, uint64_t> balance_per_subaddr = balance_per_subaddress(subaddr_account);
  std::map<uint32_t, uint64_t> unlocked_token_balance_per_subaddr = unlocked_token_balance_per_subaddress(subaddr_account);
  std::map<uint32_t, uint64_t> token_balance_per_subaddr = token_balance_per_subaddress(subaddr_account);

  if (subaddr_indices.empty()) // "index=<N1>[,<N2>,...]" wasn't specified -> use all the indices with non-zero unlocked balance
  {
    for (const auto& i : token_balance_per_subaddr)
      subaddr_indices.insert(i.first);
  }

  // early out if we know we can't make it anyway
  // we could also check for being within FEE_PER_KB, but if the fee calculation
  // ever changes, this might be missed, so let this go through
  uint64_t token_balance_subtotal = 0;
  uint64_t unlocked_token_balance_subtotal = 0;
  for (uint32_t index_minor : subaddr_indices)
  {
    token_balance_subtotal += token_balance_per_subaddr[index_minor];
    unlocked_token_balance_subtotal += unlocked_token_balance_per_subaddr[index_minor];
  }
  THROW_WALLET_EXCEPTION_IF(needed_tokens > token_balance_subtotal, error::not_enough_tokens, token_balance_subtotal, needed_tokens);
  // first check overall balance is enough, then unlocked one, so we throw distinct exceptions
  THROW_WALLET_EXCEPTION_IF(needed_tokens > unlocked_token_balance_subtotal, error::not_enough_unlocked_tokens, unlocked_token_balance_subtotal, needed_tokens);

  for (uint32_t i : subaddr_indices)
    LOG_PRINT_L2("Candidate subaddress index for spending: " << i);

  // gather all dust and non-dust outputs belonging to specified subaddresses
  size_t num_nondust_outputs = 0;
  size_t num_dust_outputs = 0;
  size_t num_nondust_token_outputs = 0;
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    const transfer_details& td = m_transfers[i];
    if (!td.m_spent && !td.m_key_image_partial && is_token_transfer_unlocked(td)
      && td.m_subaddr_index.major == subaddr_account && subaddr_indices.count(td.m_subaddr_index.minor) == 1)
    {
      const uint32_t index_minor = td.m_subaddr_index.minor;
      auto find_predicate = [&index_minor](const std::pair<uint32_t, std::vector<size_t>>& x) { return x.first == index_minor; };

      if (td.m_token_transfer && td.get_out_type() == tx_out_type::out_token )
      {
        //for token payments
        auto found = std::find_if(unused_token_transfers_indices_per_subaddr.begin(), unused_token_transfers_indices_per_subaddr.end(), find_predicate);
        if (found == unused_token_transfers_indices_per_subaddr.end())
        {
          unused_token_transfers_indices_per_subaddr.push_back({index_minor, {i}});
        }
        else
        {
          found->second.push_back(i);
        }
        ++num_nondust_token_outputs;
      }
      else
      {
        //for cash fee payment
        if ((td.is_rct()) || is_valid_decomposed_amount(td.amount()))
        {
          auto found = std::find_if(unused_transfers_indices_per_subaddr.begin(), unused_transfers_indices_per_subaddr.end(), find_predicate);
          if (found == unused_transfers_indices_per_subaddr.end())
          {
            unused_transfers_indices_per_subaddr.push_back({index_minor, {i}});
          }
          else
          {
            found->second.push_back(i);
          }
          ++num_nondust_outputs;
        }
        else
        {
          auto found = std::find_if(unused_dust_indices_per_subaddr.begin(), unused_dust_indices_per_subaddr.end(), find_predicate);
          if (found == unused_dust_indices_per_subaddr.end())
          {
            unused_dust_indices_per_subaddr.push_back({index_minor, {i}});
          }
          else
          {
            found->second.push_back(i);
          }
          ++num_dust_outputs;
        }
      }
    }
  }

  // shuffle & sort output indices
  {
    std::random_device rd;
    std::mt19937 g(rd());
    //token outputs
    std::shuffle(unused_token_transfers_indices_per_subaddr.begin(), unused_token_transfers_indices_per_subaddr.end(), g);
    std::shuffle(unused_token_dust_indices_per_subaddr.begin(), unused_token_dust_indices_per_subaddr.end(), g);
    auto sort_token_predicate = [&unlocked_token_balance_per_subaddr] (const std::pair<uint32_t, std::vector<size_t>>& x, const std::pair<uint32_t, std::vector<size_t>>& y)
    {
      return unlocked_token_balance_per_subaddr[x.first] > unlocked_token_balance_per_subaddr[y.first];
    };
    std::sort(unused_token_transfers_indices_per_subaddr.begin(), unused_token_transfers_indices_per_subaddr.end(), sort_token_predicate);
    std::sort(unused_token_dust_indices_per_subaddr.begin(), unused_token_dust_indices_per_subaddr.end(), sort_token_predicate);

    //cash outputs
    std::shuffle(unused_transfers_indices_per_subaddr.begin(), unused_transfers_indices_per_subaddr.end(), g);
    std::shuffle(unused_dust_indices_per_subaddr.begin(), unused_dust_indices_per_subaddr.end(), g);
    auto sort_cash_predicate = [&unlocked_balance_per_subaddr] (const std::pair<uint32_t, std::vector<size_t>>& x, const std::pair<uint32_t, std::vector<size_t>>& y)
    {
      return unlocked_balance_per_subaddr[x.first] > unlocked_balance_per_subaddr[y.first];
    };
    std::sort(unused_transfers_indices_per_subaddr.begin(), unused_transfers_indices_per_subaddr.end(), sort_cash_predicate);
    std::sort(unused_dust_indices_per_subaddr.begin(), unused_dust_indices_per_subaddr.end(), sort_cash_predicate);


  }

  LOG_PRINT_L2("Starting with " << num_nondust_token_outputs << " token non-dust outputs " <<
      num_nondust_outputs << " non-dust cash outputs and " << num_dust_outputs << " dust cash outputs" );

  if (unused_token_dust_indices_per_subaddr.empty() && unused_token_transfers_indices_per_subaddr.empty())
    return std::vector<wallet::pending_tx>();

  if (unused_dust_indices_per_subaddr.empty() && unused_transfers_indices_per_subaddr.empty())
    return std::vector<wallet::pending_tx>();

  // if empty, put dummy entry so that the front can be referenced later in the loop
  if (unused_token_dust_indices_per_subaddr.empty())
    unused_token_dust_indices_per_subaddr.push_back({});
  if (unused_token_transfers_indices_per_subaddr.empty())
    unused_token_transfers_indices_per_subaddr.push_back({});
  if (unused_dust_indices_per_subaddr.empty())
    unused_dust_indices_per_subaddr.push_back({});
  if (unused_transfers_indices_per_subaddr.empty())
    unused_transfers_indices_per_subaddr.push_back({});

  // start with an empty tx
  std::vector<TOKEN_TX> txes;
  txes.push_back(TOKEN_TX());

  uint64_t needed_cash_fee = 0, available_for_fee = 0; //this is safex cash
  uint64_t accumulated_fee = 0, accumulated_cash_outputs = 0, accumulated_cash_change = 0;
  uint64_t accumulated_token_outputs = 0, accumulated_token_change = 0;
  bool adding_fee = false; // true if new outputs go towards fee, rather than destinations

  std::vector<std::vector<tools::wallet::get_outs_entry>> outs;

  // while:
  // - we have something to send
  // - or we need to gather more fee
  unsigned int original_output_index = 0;
  std::vector<size_t>* unused_token_transfers_indices = &unused_token_transfers_indices_per_subaddr[0].second;
  std::vector<size_t>* unused_transfers_indices = &unused_transfers_indices_per_subaddr[0].second;
  std::vector<size_t>* unused_dust_indices      = &unused_dust_indices_per_subaddr[0].second;

  hwdev.set_mode(hw::device::TRANSACTION_CREATE_FAKE);
  while ((!dsts.empty() && dsts[0].token_amount > 0) || adding_fee) {
    TOKEN_TX &tx = txes.back();

    LOG_PRINT_L2("Start of loop with " << unused_token_transfers_indices->size());
    LOG_PRINT_L2("unused_token_transfers_indices: " << strjoin(*unused_token_transfers_indices, " "));
    LOG_PRINT_L2("unused_transfers_indices: " << strjoin(*unused_transfers_indices, " "));
    LOG_PRINT_L2("unused_dust_indices: " << strjoin(*unused_dust_indices, " "));
    LOG_PRINT_L2("dsts size " << dsts.size() << ", first " << (dsts.empty() ? "-" : cryptonote::print_money(dsts[0].token_amount)));
    LOG_PRINT_L2("adding_fee " << adding_fee);


    // if we need to spend money for fee and don't have any left, we fail
    if (adding_fee && (unused_dust_indices->empty() && unused_transfers_indices->empty()))
    {
      LOG_PRINT_L2("No more cash outputs to choose from");
      THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_balance(subaddr_account), needed_money , accumulated_fee + needed_cash_fee);

    }
    // if we need to spend tokens and don't have any left, we fail
    else if (!adding_fee && unused_token_transfers_indices->empty())
    {
      LOG_PRINT_L2("No more token outputs to choose from");
      THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_token_balance(subaddr_account), needed_tokens, accumulated_fee + needed_cash_fee);
    }

    // get a random unspent token output and use it to pay part (or all) of the current destination (and maybe next one, etc)
    // This could be more clever, but maybe at the cost of making probabilistic inferences easier
    size_t idx;
    if ((dsts.empty() || dsts[0].token_amount == 0) && !adding_fee) {
      // the "make rct txes 2/2" case - we pick a small value output to "clean up" the wallet too
      std::vector<size_t> indices;
      idx = pop_best_value(indices, tx.selected_transfers, true, tx_out_type::out_token);

      // we might not want to add it if it's a large output and we don't have many left
      if (m_transfers[idx].token_amount() >= m_min_output_value) {
        if (get_count_above(m_transfers, *unused_token_transfers_indices, m_min_output_value) < m_min_output_count) {
          LOG_PRINT_L2("Second token output was not strictly needed, and we're running out of outputs above " << print_money(m_min_output_value) << ", not adding");
          break;
        }
      }

      // since we're trying to add a second output which is not strictly needed,
      // we only add it if it's unrelated enough to the first one
      float relatedness = get_output_relatedness(m_transfers[idx], m_transfers[tx.selected_transfers.front()]);
      if (relatedness > SECOND_OUTPUT_RELATEDNESS_THRESHOLD)
      {
        LOG_PRINT_L2("Second output was not strictly needed, and relatedness " << relatedness << ", not adding");
        break;
      }
      pop_if_present(*unused_token_transfers_indices, idx);
    }
    else if (adding_fee) {
      idx = pop_best_value(unused_transfers_indices->empty() ? *unused_dust_indices : *unused_transfers_indices, tx.selected_transfers, false, tx_out_type::out_cash);
    }
    else
    {
      idx = pop_best_value(*unused_token_transfers_indices, tx.selected_transfers, true, tx_out_type::out_token);
    }

    const transfer_details &td = m_transfers[idx];
    if (adding_fee)
      LOG_PRINT_L2("Picking output " << idx << ", cash amount " << print_money(td.amount()) << ", ki " << td.m_key_image);
    else
      LOG_PRINT_L2("Picking output " << idx << ", token amount " << print_money(td.token_amount()) << ", ki " << td.m_key_image);

    // add this output to the list to spend
    uint64_t available_amount = td.amount();
    uint64_t available_token_amount = td.token_amount();
    tx.selected_transfers.push_back(idx);
    accumulated_cash_outputs += available_amount;
    accumulated_token_outputs += available_token_amount;

    // clear any fake outs we'd already gathered, since we'll need a new set
    outs.clear();

    if (adding_fee)
    {
      LOG_PRINT_L2("We need more fee, adding it to fee");
      available_for_fee += available_amount;
    }
    else
    {
      while (!dsts.empty() && dsts[0].token_amount <= available_token_amount && estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size()) < TX_SIZE_TARGET(upper_transaction_size_limit))
      {
        // we can fully pay that destination
        LOG_PRINT_L2("We can fully pay " << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
          " for " << print_money(dsts[0].token_amount));
        tx.add(dsts[0].addr, dsts[0].is_subaddress, cryptonote::tx_out_type::out_token, dsts[0].token_amount, original_output_index, m_merge_destinations);
        available_token_amount -= dsts[0].token_amount;
        dsts[0].token_amount = 0;
        pop_index(dsts, 0);
        ++original_output_index;
      }

      if (available_token_amount > 0 && !dsts.empty() && estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size()) < TX_SIZE_TARGET(upper_transaction_size_limit))
      {
        // we can partially fill that destination
        LOG_PRINT_L2("We can partially pay " << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
          " for " << print_money(available_token_amount) << "/" << print_money(dsts[0].token_amount));
        tx.add(dsts[0].addr, dsts[0].is_subaddress, cryptonote::tx_out_type::out_token, available_token_amount, original_output_index, m_merge_destinations);
        dsts[0].token_amount -= available_token_amount;
        available_token_amount = 0;
      }
    }

    // here, check if we need to sent tx and start a new one
    LOG_PRINT_L2("Considering whether to create a tx now, " << tx.selected_transfers.size() << " inputs, tx limit " << upper_transaction_size_limit);
    bool try_tx = false;
    // if we have preferred picks, but haven't yet used all of them, continue
    if (adding_fee)
    {
      /* might not actually be enough if adding this output bumps size to next kB, but we need to try */
      try_tx = available_for_fee >= needed_cash_fee;
    }
    else
    {
      const size_t estimated_tx_size = estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size());
      try_tx = dsts.empty() || (estimated_tx_size >= TX_SIZE_TARGET(upper_transaction_size_limit));
    }


    if (try_tx) {
      cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
      pending_tx test_ptx = AUTO_VAL_INIT(test_ptx);

      //Now, we can calculate fee, and go back one more round to select cash
      //inputs to pay that fee
      const size_t estimated_tx_size = estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size());
      needed_cash_fee = calculate_fee(fee_per_kb, estimated_tx_size, fee_multiplier);

      uint64_t inputs = 0, outputs = needed_cash_fee;
      for (size_t idx: tx.selected_transfers)
      {
        inputs += m_transfers[idx].amount();
      }
      for (const auto &o: tx.dsts)
      {
        outputs += o.amount; //shoud be zero, only token outputs
      }

      if (inputs == 0 || inputs < outputs)
      {
        LOG_PRINT_L2("Switching to adding_fee mode for token transaction");
        adding_fee = true;
        goto skip_tx;
      }

      LOG_PRINT_L2("Trying to create a tx now, with " << tx.dsts.size() << " outputs and " <<
        tx.selected_transfers.size() << " inputs");
      transfer_selected<detail::split_strategy_function, cryptonote::tx_out_type::out_token>(tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_cash_fee, extra,
          detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);
      auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
      needed_cash_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
      available_for_fee = test_ptx.fee + test_ptx.change_dts.amount + (!test_ptx.dust_added_to_fee ? test_ptx.dust : 0);
      LOG_PRINT_L2("Made a " << get_size_string(txBlob) << " tx, with " << print_money(available_for_fee) << " available for fee (" <<
        print_money(needed_cash_fee) << " needed)");

      if (needed_cash_fee > available_for_fee && !dsts.empty() && dsts[0].amount > 0)
      {
        // we don't have enough for the fee, but we've only partially paid the current address,
        // so we can take the fee from the paid amount, since we'll have to make another tx anyway
        std::vector<cryptonote::tx_destination_entry>::iterator i;
        i = std::find_if(tx.dsts.begin(), tx.dsts.end(),
          [&](const cryptonote::tx_destination_entry &d) { return !memcmp (&d.addr, &dsts[0].addr, sizeof(dsts[0].addr)); });
        THROW_WALLET_EXCEPTION_IF(i == tx.dsts.end(), error::wallet_internal_error, "paid address not found in outputs");
        if (i->amount > needed_cash_fee)
        {
          uint64_t new_paid_amount = i->amount /*+ test_ptx.fee*/ - needed_cash_fee;
          LOG_PRINT_L2("Adjusting amount paid to " << get_account_address_as_str(m_nettype, i->is_subaddress, i->addr) << " from " <<
            print_money(i->amount) << " to " << print_money(new_paid_amount) << " to accommodate " <<
            print_money(needed_cash_fee) << " fee");
          dsts[0].amount += i->amount - new_paid_amount;
          i->amount = new_paid_amount;
          test_ptx.fee = needed_cash_fee;
          available_for_fee = needed_cash_fee;
        }
      }

      if (needed_cash_fee > available_for_fee)
      {
        LOG_PRINT_L2("We could not make a tx, switching to fee accumulation");

        adding_fee = true;
      }
      else
      {
        LOG_PRINT_L2("We made a tx, adjusting fee and saving it, we need " << print_money(needed_cash_fee) << " and we have " << print_money(test_ptx.fee));
        while (needed_cash_fee > test_ptx.fee) {
          transfer_selected(tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_cash_fee, extra,
              detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);
          txBlob = t_serializable_object_to_blob(test_ptx.tx);
          needed_cash_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
          LOG_PRINT_L2("Made an attempt at a  final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
            " fee  and " << print_money(test_ptx.change_dts.amount) << " cash change and" << print_money(test_ptx.change_token_dts.token_amount) << " token change");
        }

        LOG_PRINT_L2("Made a final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
          " fee  and " << print_money(test_ptx.change_dts.amount) << " cash change and" << print_money(test_ptx.change_token_dts.token_amount) << " token change");

        tx.tx = test_tx;
        tx.ptx = test_ptx;
        tx.bytes = txBlob.size();
        tx.outs = outs;
        accumulated_fee += test_ptx.fee;
        accumulated_cash_change += test_ptx.change_dts.amount;
        accumulated_token_change += test_ptx.change_token_dts.token_amount;
        adding_fee = false;
        if (!dsts.empty()) {
          LOG_PRINT_L2("We have more to pay, starting another tx");
          txes.push_back(TOKEN_TX());
          original_output_index = 0;
          needed_cash_fee = 0;
        }
      }
    }

skip_tx:
    // if unused_*_indices is empty while unused_*_indices_per_subaddr has multiple elements, and if we still have something to pay,
    // pop front of unused_*_indices_per_subaddr and have unused_*_indices point to the front of unused_*_indices_per_subaddr
    if ((!dsts.empty() && dsts[0].token_amount > 0) && (!adding_fee))
    {
      if (unused_token_transfers_indices->empty() && unused_token_transfers_indices_per_subaddr.size() > 1)
      {
        unused_token_transfers_indices_per_subaddr.erase(unused_token_transfers_indices_per_subaddr.begin());
        unused_token_transfers_indices = &unused_token_transfers_indices_per_subaddr[0].second;
      }
      if (unused_token_dust_indices_per_subaddr.size() > 1)
      {
        unused_token_dust_indices_per_subaddr.erase(unused_token_dust_indices_per_subaddr.begin());
      }
    }

    //Cash indices, for fee
    if (adding_fee)
    {
      if (unused_transfers_indices->empty() && unused_transfers_indices_per_subaddr.size() > 1)
      {
        unused_transfers_indices_per_subaddr.erase(unused_transfers_indices_per_subaddr.begin());
        unused_transfers_indices = &unused_transfers_indices_per_subaddr[0].second;
      }
      if (unused_dust_indices->empty() && unused_dust_indices_per_subaddr.size() > 1)
      {
        unused_dust_indices_per_subaddr.erase(unused_dust_indices_per_subaddr.begin());
        unused_dust_indices = &unused_dust_indices_per_subaddr[0].second;
      }
    }
  }

  if (adding_fee)
  {
    LOG_PRINT_L1("We ran out of outputs while trying to gather final fee");
    THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_balance(subaddr_account), needed_money, accumulated_fee + needed_cash_fee);
  }

  LOG_PRINT_L1("Done creating " << txes.size() << " transactions, " << print_money(accumulated_fee) <<
    " total fee, " << print_money(accumulated_cash_change) << " total cash change and " << print_money(accumulated_token_change) << " total token change");

  hwdev.set_mode(hw::device::TRANSACTION_CREATE_REAL);
  for (std::vector<TOKEN_TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TOKEN_TX &tx = *i;
    cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
    pending_tx test_ptx = AUTO_VAL_INIT(test_ptx);

    transfer_selected(tx.dsts,
                      tx.selected_transfers,
                      fake_outs_count,
                      tx.outs,
                      unlock_time,
                      tx.ptx.fee,
                      extra,
                      detail::digit_split_strategy,
                      tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD),
                      test_tx,
                      test_ptx);

    auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
    tx.tx = test_tx;
    tx.ptx = test_ptx;
    tx.bytes = txBlob.size();
  }

  std::vector<wallet::pending_tx> ptx_vector;
  for (std::vector<TOKEN_TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TOKEN_TX &tx = *i;
    uint64_t tx_money = 0;
    uint64_t tx_tokens = 0;
    for (size_t idx: tx.selected_transfers)
      tx_money += m_transfers[idx].amount();
    for (size_t idx: tx.selected_transfers)
      tx_tokens += m_transfers[idx].token_amount();
    LOG_PRINT_L1("  Transaction " << (1+std::distance(txes.begin(), i)) << "/" << txes.size() <<
      ": " << get_size_string(tx.bytes) << ", sending " << print_money(tx_tokens) << " tokens and" ", sending " << print_money(tx_money) << " cash in " << tx.selected_transfers.size() <<
      " outputs to " << tx.dsts.size() << " destination(s), including " <<
      print_money(tx.ptx.fee) << " fee, " << print_money(tx.ptx.change_token_dts.token_amount) << " token change and " << print_money(tx.ptx.change_dts.amount) << " cash change");
    ptx_vector.push_back(tx.ptx);
  }

  // if we made it this far, we're OK to actually send the transactions
  return ptx_vector;
}

std::vector<wallet::pending_tx> wallet::create_transactions_all(uint64_t below, const cryptonote::account_public_address &address, bool is_subaddress, const size_t fake_outs_count, const uint64_t unlock_time, uint32_t priority, const std::vector<uint8_t>& extra, uint32_t subaddr_account, std::set<uint32_t> subaddr_indices, bool trusted_daemon)
{
  std::vector<size_t> unused_transfers_indices;
  std::vector<size_t> unused_dust_indices;

  THROW_WALLET_EXCEPTION_IF(unlocked_balance(subaddr_account) == 0, error::wallet_internal_error, "No unlocked balance in the entire wallet");

  std::map<uint32_t, std::pair<std::vector<size_t>, std::vector<size_t>>> unused_transfer_dust_indices_per_subaddr;

  // gather all dust and non-dust outputs of specified subaddress (if any) and below specified threshold (if any)
  bool fund_found = false;
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    const transfer_details& td = m_transfers[i];
    if (!td.m_spent && !td.m_key_image_partial && is_transfer_unlocked(td) && td.m_subaddr_index.major == subaddr_account && (subaddr_indices.empty() || subaddr_indices.count(td.m_subaddr_index.minor) == 1))
    {
      fund_found = true;
      if (below == 0 || td.amount() < below)
      {
        if ((td.is_rct()) || is_valid_decomposed_amount(td.amount()))
          unused_transfer_dust_indices_per_subaddr[td.m_subaddr_index.minor].first.push_back(i);
        else
          unused_transfer_dust_indices_per_subaddr[td.m_subaddr_index.minor].second.push_back(i);
      }
    }
  }
  THROW_WALLET_EXCEPTION_IF(!fund_found, error::wallet_internal_error, "No unlocked balance in the specified subaddress(es)");
  THROW_WALLET_EXCEPTION_IF(unused_transfer_dust_indices_per_subaddr.empty(), error::wallet_internal_error, "The smallest amount found is not below the specified threshold");

  if (subaddr_indices.empty())
  {
    // in case subaddress index wasn't specified, choose non-empty subaddress randomly (with index=0 being chosen last)
    if (unused_transfer_dust_indices_per_subaddr.count(0) == 1 && unused_transfer_dust_indices_per_subaddr.size() > 1)
      unused_transfer_dust_indices_per_subaddr.erase(0);
    auto i = unused_transfer_dust_indices_per_subaddr.begin();
    std::advance(i, crypto::rand<size_t>() % unused_transfer_dust_indices_per_subaddr.size());
    unused_transfers_indices = i->second.first;
    unused_dust_indices = i->second.second;
    LOG_PRINT_L2("Spending from subaddress index " << i->first);
  }
  else
  {
    for (const auto& p : unused_transfer_dust_indices_per_subaddr)
    {
      unused_transfers_indices.insert(unused_transfers_indices.end(), p.second.first.begin(), p.second.first.end());
      unused_dust_indices.insert(unused_dust_indices.end(), p.second.second.begin(), p.second.second.end());
      LOG_PRINT_L2("Spending from subaddress index " << p.first);
    }
  }

  return create_transactions_from(address, is_subaddress, unused_transfers_indices, unused_dust_indices, fake_outs_count, unlock_time, priority, extra, trusted_daemon);
}

std::vector<wallet::pending_tx> wallet::create_transactions_single(const crypto::key_image &ki, const cryptonote::account_public_address &address, bool is_subaddress, const size_t fake_outs_count, const uint64_t unlock_time, uint32_t priority, const std::vector<uint8_t>& extra, bool trusted_daemon)
{
  std::vector<size_t> unused_transfers_indices;
  std::vector<size_t> unused_dust_indices;
  // find output with the given key image
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    const transfer_details& td = m_transfers[i];
    if (td.m_key_image_known && td.m_key_image == ki && !td.m_spent && is_transfer_unlocked(td))
    {
      if (td.is_rct() || is_valid_decomposed_amount(td.amount()))
        unused_transfers_indices.push_back(i);
      else
        unused_dust_indices.push_back(i);
      break;
    }
  }
  return create_transactions_from(address, is_subaddress, unused_transfers_indices, unused_dust_indices, fake_outs_count, unlock_time, priority, extra, trusted_daemon);
}

std::vector<wallet::pending_tx> wallet::create_transactions_migration(
    std::vector<cryptonote::tx_destination_entry> dsts,
    const crypto::hash bitcoin_transaction_hash, const uint64_t unlock_time,
    uint32_t priority, const std::vector<uint8_t> &extra, bool trusted_daemon,
    bool mark_as_spent)
{
  const size_t fake_outs_count = 0;
  const uint64_t fee_per_kb  = get_per_kb_fee();
  const uint64_t fee_multiplier = get_fee_multiplier(priority, get_fee_algorithm());

  const uint64_t needed_cash = [&]() {
    uint64_t tmp = 0;
    for (auto &dt: dsts) {
      tmp += dt.amount;
      THROW_WALLET_EXCEPTION_IF(tmp < dt.amount, error::tx_sum_overflow, dsts, 0, m_nettype);
    }
    return tmp;}();


  // failsafe split attempt counter
  size_t attempt_count = 0;
  for (attempt_count = 1;; attempt_count++)
  {
    size_t num_tx = 1;
    auto split_values = split_amounts(dsts, num_tx);

    // Throw if split_amounts comes back with a vector of size different than it should
    if (split_values.size() != num_tx)
    {
      throw std::runtime_error("Splitting transactions returned a number of potential tx not equal to what was requested");
    }

    std::vector<pending_tx> ptx_vector;
    try
    {
      // for each new destination vector (i.e. for each new tx)
      for (auto &dst_vector : split_values)
      {
        cryptonote::transaction tx;
        pending_tx ptx = AUTO_VAL_INIT(ptx);
        std::vector<size_t> unused_transfers_indices = select_available_outputs_from_histogram(fake_outs_count + 1, true, true, false, trusted_daemon, cryptonote::tx_out_type::out_cash);

        size_t idx;
        uint64_t found_cash = 0;
        uint64_t min_needed_cash = needed_cash;
        bool adding_fee = false;
        uint64_t needed_fee = 0;
        std::vector<size_t> selected_transfers;
        while (found_cash < min_needed_cash && !adding_fee) {
          //Select cash inputs for migration cash distribution
          idx = pop_best_value(unused_transfers_indices, selected_transfers, false, tx_out_type::out_cash);
          const transfer_details &td = m_transfers[idx];

          found_cash += td.amount();
          selected_transfers.push_back(idx);
          if (found_cash > min_needed_cash && !adding_fee) {
              adding_fee = true;
              const size_t estimated_tx_size = estimate_tx_size(selected_transfers.size(), fake_outs_count, dst_vector.size(), extra.size());
              needed_fee = calculate_fee(fee_per_kb, estimated_tx_size, fee_multiplier);
              min_needed_cash += needed_fee;
          }
        }

        do
        {
          transfer_migration(dst_vector, bitcoin_transaction_hash, fake_outs_count, selected_transfers,
                             unlock_time, needed_fee, extra, tx, ptx, trusted_daemon);
          auto txBlob = t_serializable_object_to_blob(ptx.tx);
          needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
        } while (ptx.fee < needed_fee);

        ptx_vector.push_back(ptx);

        // mark transfers to be used as "spent"
        for(size_t idx: ptx.selected_transfers)
        {
          set_spent(idx, 0);
        }
      }

      // if we made it this far, we've selected our transactions.  committing them will mark them spent,
      // so this is a failsafe in case they don't go through
      // unmark pending tx transfers as spent
      if (!mark_as_spent) {
        for (auto &ptx : ptx_vector) {
          // mark transfers to be used as not spent
          for (size_t idx2: ptx.selected_transfers) {
            set_unspent(idx2);
          }

        }
      }

      // if we made it this far, we're OK to actually send the transactions
      return ptx_vector;

    }
    // only catch this here, other exceptions need to pass through to the calling function
    catch (const tools::error::tx_too_big& e)
    {

      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        for(size_t idx2: ptx.selected_transfers)
        {
          set_unspent(idx2);
        }
      }

      if (attempt_count >= MAX_SPLIT_ATTEMPTS)
      {
        throw;
      }
    }
    catch (...)
    {
      // in case of some other exception, make sure any tx in queue are marked unspent again

      // unmark pending tx transfers as spent
      for (auto & ptx : ptx_vector)
      {
        // mark transfers to be used as not spent
        for(size_t idx2: ptx.selected_transfers)
        {
          set_unspent(idx2);
        }
      }

      throw;
    }
  }
}


std::vector<wallet::pending_tx> wallet::create_transactions_advanced(safex::command_t command_type, std::vector<cryptonote::tx_destination_entry> dsts,
        const size_t fake_outs_count, const uint64_t unlock_time, uint32_t priority, const std::vector<uint8_t> &extra,
        uint32_t subaddr_account, std::set<uint32_t> subaddr_indices, bool trusted_daemon, const safex::safex_account &sfx_acc)
  {
    //ensure device is let in NONE mode in any case
    hw::device &hwdev = m_account.get_device();
    boost::unique_lock<hw::device> hwdev_lock(hwdev);
    hw::reset_mode rst(hwdev);

    THROW_WALLET_EXCEPTION_IF(m_light_wallet, error::not_supported);

    std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_token_transfers_indices_per_subaddr;
    std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_cash_transfers_indices_per_subaddr;
    std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_cash_dust_indices_per_subaddr;
    std::vector<std::pair<uint32_t, std::vector<size_t>>> unused_staked_token_transfers_indices_per_subaddr;

    struct ADVANCED_TX
    {
      std::vector<size_t> selected_transfers;
      std::vector<cryptonote::tx_destination_entry> dsts;
      cryptonote::transaction tx;
      pending_tx ptx = AUTO_VAL_INIT(ptx);
      size_t bytes = 0;
      std::vector<std::vector<tools::wallet::get_outs_entry>> outs;

      void add(const account_public_address &addr, bool is_subaddress, cryptonote::tx_out_type output_type, uint64_t amount, uint64_t token_amount,
              unsigned int original_output_index, bool merge_destinations, const cryptonote::blobdata& output_data={})
      {
        if (merge_destinations)
        {
          std::vector<cryptonote::tx_destination_entry>::iterator i;
          i = std::find_if(dsts.begin(), dsts.end(), [&](const cryptonote::tx_destination_entry &d)
          { return !memcmp(&d.addr, &addr,sizeof(addr)) && (d.output_type == tx_out_type::out_cash || d.output_type == tx_out_type::out_token); }); //merge only cash and token outputs

          if (i == dsts.end())
          {
            dsts.emplace_back(0, addr, is_subaddress, output_type, output_data);
            i = dsts.end() - 1;
          }

          THROW_WALLET_EXCEPTION_IF((output_type == cryptonote::tx_out_type::out_token) && !tools::is_whole_token_amount(amount), error::wallet_internal_error, "Token amount must be whole number.");
          i->token_amount += token_amount;
          i->amount += amount;

        }
        else
        {
          THROW_WALLET_EXCEPTION_IF(original_output_index > dsts.size(), error::wallet_internal_error,
                                    std::string("original_output_index too large: ") + std::to_string(original_output_index) + " > " + std::to_string(dsts.size()));
          if (original_output_index == dsts.size())
            dsts.emplace_back(0, addr, is_subaddress, output_type, output_data);
            THROW_WALLET_EXCEPTION_IF(memcmp(&dsts[original_output_index].addr, &addr, sizeof(addr)), error::wallet_internal_error, "Mismatched destination address");
            THROW_WALLET_EXCEPTION_IF((output_type == cryptonote::tx_out_type::out_token) && !tools::is_whole_token_amount(amount), error::wallet_internal_error,
                    "Token amount must be whole number.");
            dsts[original_output_index].token_amount += token_amount;
            dsts[original_output_index].amount += amount;
        }
      }
    };

    uint64_t upper_transaction_size_limit = get_upper_transaction_size_limit();

    const uint64_t fee_per_kb = get_per_kb_fee();
    const uint64_t fee_multiplier = get_fee_multiplier(priority, get_fee_algorithm());

    // throw if attempting a transaction with no destinations
    THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);


    // calculate total amount being sent to all destinations
    // throw if total amount overflows uint64_t
    uint64_t needed_cash = 0;
    uint64_t needed_tokens = 0;
    uint64_t needed_staked_tokens = 0;
    for (auto &dt: dsts)
    {
      if ((command_type == safex::command_t::token_stake) || (command_type == safex::command_t::token_unstake))
      {
        THROW_WALLET_EXCEPTION_IF(0 == dt.token_amount, error::zero_destination);
        THROW_WALLET_EXCEPTION_IF(!tools::is_whole_token_amount(dt.token_amount), error::wallet_internal_error, "Token amount must be a round number.");

        if (command_type == safex::command_t::token_stake)
        {
          LOG_PRINT_L2("transfer: adding " << print_money(dt.token_amount) << " tokens for token staking, for a total of " << print_money(needed_tokens)) << " tokens";
          needed_tokens += dt.token_amount;
          THROW_WALLET_EXCEPTION_IF(needed_tokens < dt.token_amount, error::tx_sum_overflow, dsts, 0, m_nettype);
        }
        else
        {
          LOG_PRINT_L2("transfer: adding " << print_money(dt.token_amount) << " tokens for token unstaking, for a total of " << print_money(needed_tokens)) << " staked tokens";
          needed_staked_tokens += dt.token_amount;
          THROW_WALLET_EXCEPTION_IF(needed_staked_tokens < dt.token_amount, error::tx_sum_overflow, dsts, 0, m_nettype);
        }

      }
      else if (command_type == safex::command_t::donate_network_fee || command_type == safex::command_t::simple_purchase)
      {
          THROW_WALLET_EXCEPTION_IF(0 == dt.amount, error::zero_destination);
          needed_cash += dt.amount;
          LOG_PRINT_L2("transfer: donating " << print_money(dt.amount) << " safex cash to safex token holders, for a total of " << print_money(needed_cash) << " cash");
          THROW_WALLET_EXCEPTION_IF(needed_tokens < dt.token_amount, error::tx_sum_overflow, dsts, 0, m_nettype);
      }
      else if (command_type == safex::command_t::create_account)
      {
        if (dt.script_output == false) {
          THROW_WALLET_EXCEPTION_IF(dt.token_amount != SAFEX_CREATE_ACCOUNT_TOKEN_LOCK_FEE, error::zero_destination);
          needed_tokens += dt.token_amount;
          LOG_PRINT_L2("creting account: locking " << print_money(dt.token_amount) << " safex token" << " cash");
          THROW_WALLET_EXCEPTION_IF(needed_tokens < dt.token_amount, error::tx_sum_overflow, dsts, 0, m_nettype);
        } else {
          THROW_WALLET_EXCEPTION_IF(dt.output_type != tx_out_type::out_safex_account, error::safex_invalid_output_error);
        }

      }
      else if (command_type == safex::command_t::edit_account)
      {
        THROW_WALLET_EXCEPTION_IF(dt.output_type != tx_out_type::out_safex_account_update, error::safex_invalid_output_error);
      }
      else
      {
        THROW_WALLET_EXCEPTION_IF(dsts.empty(), error::zero_destination);
      }
    }

    // throw if attempting a transaction with no money
    THROW_WALLET_EXCEPTION_IF((command_type == safex::command_t::token_stake || command_type == safex::command_t::create_account) && needed_tokens == 0, error::zero_destination);
    THROW_WALLET_EXCEPTION_IF(command_type == safex::command_t::token_unstake && needed_staked_tokens == 0, error::zero_destination);
    THROW_WALLET_EXCEPTION_IF(command_type == safex::command_t::donate_network_fee && needed_cash == 0, error::zero_destination);

    std::map<uint32_t, uint64_t> unlocked_cash_balance_per_subaddr = unlocked_balance_per_subaddress(subaddr_account);
    std::map<uint32_t, uint64_t> cash_balance_per_subaddr = balance_per_subaddress(subaddr_account);
    std::map<uint32_t, uint64_t> unlocked_token_balance_per_subaddr = unlocked_token_balance_per_subaddress(subaddr_account);
    std::map<uint32_t, uint64_t> token_balance_per_subaddr = token_balance_per_subaddress(subaddr_account);
    std::map<uint32_t, uint64_t> unlocked_staked_token_balance_per_subaddr = unlocked_staked_token_balance_per_subaddress(subaddr_account);
    std::map<uint32_t, uint64_t> staked_token_balance_per_subaddr = staked_token_balance_per_subaddress(subaddr_account);

    if (subaddr_indices.empty()) // "index=<N1>[,<N2>,...]" wasn't specified -> use all the indices with non-zero unlocked balance
    {
      for (const auto &i : token_balance_per_subaddr)
        subaddr_indices.insert(i.first);
    }

    uint64_t cash_balance_subtotal = 0;
    uint64_t unlocked_cash_balance_subtotal = 0;
    for (uint32_t index_minor : subaddr_indices)
    {
      cash_balance_subtotal += cash_balance_per_subaddr[index_minor];
      unlocked_cash_balance_subtotal += unlocked_cash_balance_per_subaddr[index_minor];
    }

    // early out if we know we can't make it anyway
    // we could also check for being within FEE_PER_KB, but if the fee calculation
    // ever changes, this might be missed, so let this go through
    uint64_t token_balance_subtotal = 0;
    uint64_t unlocked_token_balance_subtotal = 0;
    for (uint32_t index_minor : subaddr_indices)
    {
      token_balance_subtotal += token_balance_per_subaddr[index_minor];
      unlocked_token_balance_subtotal += unlocked_token_balance_per_subaddr[index_minor];
    }

    uint64_t staked_token_balance_subtotal = 0;
    uint64_t unlocked_staked_token_balance_subtotal = 0; //funny name
    if (command_type == safex::command_t::token_unstake) //relevant only for this command
    {
      for (uint32_t index_minor : subaddr_indices)
      {
        staked_token_balance_subtotal += staked_token_balance_per_subaddr[index_minor];
        unlocked_staked_token_balance_subtotal += unlocked_staked_token_balance_per_subaddr[index_minor];
      }

      THROW_WALLET_EXCEPTION_IF(needed_staked_tokens > staked_token_balance_subtotal, error::not_enough_staked_tokens, staked_token_balance_subtotal, needed_staked_tokens);
      THROW_WALLET_EXCEPTION_IF(needed_staked_tokens > unlocked_staked_token_balance_subtotal, error::not_enough_unlocked_tokens, unlocked_staked_token_balance_subtotal, needed_staked_tokens);
    }


    THROW_WALLET_EXCEPTION_IF(needed_cash > cash_balance_subtotal, error::not_enough_cash, cash_balance_subtotal, needed_cash, 0);
    THROW_WALLET_EXCEPTION_IF(needed_cash > unlocked_cash_balance_subtotal, error::not_enough_unlocked_cash, unlocked_cash_balance_subtotal, needed_cash, 0);
    THROW_WALLET_EXCEPTION_IF(needed_tokens > token_balance_subtotal, error::not_enough_tokens, token_balance_subtotal, needed_tokens);
    THROW_WALLET_EXCEPTION_IF(needed_tokens > unlocked_token_balance_subtotal, error::not_enough_unlocked_tokens, unlocked_token_balance_subtotal, needed_tokens);


    for (uint32_t i : subaddr_indices)
      LOG_PRINT_L2("Candidate subaddress index for spending: " << i);

    // gather all dust and non-dust outputs belonging to specified subaddresses
    size_t num_nondust_outputs = 0;
    size_t num_dust_outputs = 0;
    size_t num_nondust_token_outputs = 0;
    size_t num_nondust_staked_token_outputs = 0;
    for (size_t i = 0; i < m_transfers.size(); ++i)
    {
      const transfer_details &td = m_transfers[i];
      if (!td.m_spent && !td.m_key_image_partial && is_transfer_unlocked(td) && td.m_subaddr_index.major == subaddr_account && subaddr_indices.count(td.m_subaddr_index.minor) == 1)
      {
        const uint32_t index_minor = td.m_subaddr_index.minor;
        auto find_predicate = [&index_minor](const std::pair<uint32_t, std::vector<size_t>> &x)
        { return x.first == index_minor; };

        if ((command_type == safex::command_t::token_unstake) && td.m_output_type == cryptonote::tx_out_type::out_staked_token)
        {
          auto found = std::find_if(unused_staked_token_transfers_indices_per_subaddr.begin(), unused_staked_token_transfers_indices_per_subaddr.end(), find_predicate);
          if (found == unused_staked_token_transfers_indices_per_subaddr.end())
          {
            unused_staked_token_transfers_indices_per_subaddr.push_back({index_minor, {i}});
          }
          else
          {
            found->second.push_back(i);
          }
          ++num_nondust_staked_token_outputs;
        }
        else if (td.token_amount() > 0 &&  td.m_output_type == cryptonote::tx_out_type::out_token)
        {
          auto found = std::find_if(unused_token_transfers_indices_per_subaddr.begin(), unused_token_transfers_indices_per_subaddr.end(), find_predicate);
          if (found == unused_token_transfers_indices_per_subaddr.end())
          {
            unused_token_transfers_indices_per_subaddr.push_back({index_minor, {i}});
          }
          else
          {
            found->second.push_back(i);
          }
          ++num_nondust_token_outputs;
        }

        //for cash fee payment
        if (td.amount() > 0 && td.m_output_type == cryptonote::tx_out_type::out_cash)
        {
          if (is_valid_decomposed_amount(td.amount()))
          {
            auto found = std::find_if(unused_cash_transfers_indices_per_subaddr.begin(), unused_cash_transfers_indices_per_subaddr.end(), find_predicate);
            if (found == unused_cash_transfers_indices_per_subaddr.end())
            {
              unused_cash_transfers_indices_per_subaddr.push_back({index_minor, {i}});
            }
            else
            {
              found->second.push_back(i);
            }
            ++num_nondust_outputs;
          }
          else
          {
            auto found = std::find_if(unused_cash_dust_indices_per_subaddr.begin(), unused_cash_dust_indices_per_subaddr.end(), find_predicate);
            if (found == unused_cash_dust_indices_per_subaddr.end())
            {
              unused_cash_dust_indices_per_subaddr.push_back({index_minor, {i}});
            }
            else
            {
              found->second.push_back(i);
            }
            ++num_dust_outputs;
          }
        }
      }
    }

    // shuffle & sort output indices
    {
      std::random_device rd;
      std::mt19937 g(rd());

      if (command_type == safex::command_t::token_unstake) {
        //staked token outputs
        std::shuffle(unused_staked_token_transfers_indices_per_subaddr.begin(), unused_staked_token_transfers_indices_per_subaddr.end(), g);
        auto sort_token_predicate = [&unlocked_staked_token_balance_per_subaddr](const std::pair<uint32_t, std::vector<size_t>> &x, const std::pair<uint32_t, std::vector<size_t>> &y)
        {
          return unlocked_staked_token_balance_per_subaddr[x.first] > unlocked_staked_token_balance_per_subaddr[y.first];
        };
        std::sort(unused_staked_token_transfers_indices_per_subaddr.begin(), unused_staked_token_transfers_indices_per_subaddr.end(), sort_token_predicate);

        if (unused_staked_token_transfers_indices_per_subaddr.empty())
          return std::vector<wallet::pending_tx>();

      } else if (command_type == safex::command_t::token_stake || command_type == safex::command_t::token_collect || command_type == safex::command_t::create_account) {
        //shuffle common token outputs
        std::shuffle(unused_token_transfers_indices_per_subaddr.begin(), unused_token_transfers_indices_per_subaddr.end(), g);
        auto sort_token_predicate = [&unlocked_token_balance_per_subaddr](const std::pair<uint32_t, std::vector<size_t>> &x, const std::pair<uint32_t, std::vector<size_t>> &y)
        {
          return unlocked_token_balance_per_subaddr[x.first] > unlocked_token_balance_per_subaddr[y.first];
        };
        std::sort(unused_token_transfers_indices_per_subaddr.begin(), unused_token_transfers_indices_per_subaddr.end(), sort_token_predicate);

        if (unused_token_transfers_indices_per_subaddr.empty())
          return std::vector<wallet::pending_tx>();
      }

      //cash outputs
      std::shuffle(unused_cash_transfers_indices_per_subaddr.begin(), unused_cash_transfers_indices_per_subaddr.end(), g);
      std::shuffle(unused_cash_dust_indices_per_subaddr.begin(), unused_cash_dust_indices_per_subaddr.end(), g);
      auto sort_cash_predicate = [&unlocked_cash_balance_per_subaddr](const std::pair<uint32_t, std::vector<size_t>> &x, const std::pair<uint32_t, std::vector<size_t>> &y)
      {
        return unlocked_cash_balance_per_subaddr[x.first] > unlocked_cash_balance_per_subaddr[y.first];
      };
      std::sort(unused_cash_transfers_indices_per_subaddr.begin(), unused_cash_transfers_indices_per_subaddr.end(), sort_cash_predicate);
      std::sort(unused_cash_dust_indices_per_subaddr.begin(), unused_cash_dust_indices_per_subaddr.end(), sort_cash_predicate);
    }

    LOG_PRINT_L2("Starting with " << num_nondust_token_outputs << " token non-dust outputs and " <<
                                  num_nondust_outputs << " non-dust cash outputs and " << num_dust_outputs << " dust cash outputs");


    if (unused_cash_dust_indices_per_subaddr.empty() && unused_cash_transfers_indices_per_subaddr.empty())
      return std::vector<wallet::pending_tx>();

    // if empty, put dummy entry so that the front can be referenced later in the loop
    if (unused_token_transfers_indices_per_subaddr.empty())
      unused_token_transfers_indices_per_subaddr.push_back({});
    if (unused_cash_dust_indices_per_subaddr.empty())
      unused_cash_dust_indices_per_subaddr.push_back({});
    if (unused_cash_transfers_indices_per_subaddr.empty())
      unused_cash_transfers_indices_per_subaddr.push_back({});
    if (unused_staked_token_transfers_indices_per_subaddr.empty())
      unused_staked_token_transfers_indices_per_subaddr.push_back({});

    // start with an empty tx
    std::vector<ADVANCED_TX> txes;
    txes.push_back(ADVANCED_TX());

    uint64_t needed_fee = 0, available_for_fee = 0; //this is safex cash
    uint64_t accumulated_cash_fee = 0, accumulated_cash_outputs = 0, accumulated_cash_change = 0;
    uint64_t accumulated_token_outputs = 0, accumulated_token_change = 0;
    uint64_t accumulated_staked_token_outputs = 0;

    bool adding_fee = false; // true if new outputs go towards fee, rather than destinations

    std::vector<std::vector<tools::wallet::get_outs_entry>> outs;

    // while:
    // - we have something to send
    // - or we need to gather more fee
    unsigned int original_output_index = 0;
    std::vector<size_t> *unused_token_transfers_indices = &unused_token_transfers_indices_per_subaddr[0].second;
    std::vector<size_t> *unused_cash_transfers_indices = &unused_cash_transfers_indices_per_subaddr[0].second;
    std::vector<size_t> *unused_cash_dust_indices = &unused_cash_dust_indices_per_subaddr[0].second;
    std::vector<size_t> *unused_staked_token_transfers_indices = &unused_staked_token_transfers_indices_per_subaddr[0].second;


    hwdev.set_mode(hw::device::TRANSACTION_CREATE_FAKE);
    while ((!dsts.empty() && (dsts[0].token_amount > 0 || dsts[0].amount > 0
                              || dsts[0].output_type == tx_out_type::out_safex_account
                              || dsts[0].output_type == tx_out_type::out_safex_account_update)) || adding_fee)
    {
      ADVANCED_TX &tx = txes.back();

      LOG_PRINT_L2("Start of loop with tokens: " << unused_token_transfers_indices->size() << " staked_tokens: " << unused_staked_token_transfers_indices->size());
      LOG_PRINT_L2("unused_token_transfers_indices: " << (unused_token_transfers_indices->size() < 100 ? strjoin(*unused_token_transfers_indices, " ") : " too big to print"));
      LOG_PRINT_L2("unused_staked_token_transfers_indices: " << (unused_staked_token_transfers_indices->size() < 100 ? strjoin(*unused_staked_token_transfers_indices, " ") : "too big to print"));
      LOG_PRINT_L2("unused_cash_transfers_indices: " << (unused_cash_transfers_indices->size() < 100 ? strjoin(*unused_cash_transfers_indices, "too big to print") : " "));
      LOG_PRINT_L2("unused_cash_dust_indices: " << (unused_cash_dust_indices->size() < 100 ? strjoin(*unused_cash_dust_indices, " ") : "too big to print"));
      LOG_PRINT_L2("dsts size " << dsts.size() << ", first " << (dsts.empty() ? "-" : cryptonote::print_money(dsts[0].token_amount)));
      LOG_PRINT_L2("adding_fee " << adding_fee);

      const bool advanced_output_reference = (dsts[0].output_type == tx_out_type::out_safex_account_update);


      // if we need to spend cash and don't have any left, we fail
      if ((adding_fee || needed_cash>0) && (unused_cash_dust_indices->empty() && unused_cash_transfers_indices->empty()))
      {
        LOG_PRINT_L2("No more cash outputs to choose from");
        THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_balance(subaddr_account), needed_cash, accumulated_cash_fee + needed_fee);

      }
        // if we need to spend tokens and don't have any left, we fail
      else if (!adding_fee && needed_tokens > 0 && unused_token_transfers_indices->empty())
      {
        LOG_PRINT_L2("No more token outputs to choose from");
        THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_token_balance(subaddr_account), needed_tokens, accumulated_cash_fee + needed_fee);
      }
      else if (!adding_fee && needed_staked_tokens > 0 && unused_staked_token_transfers_indices->empty())
      {
        LOG_PRINT_L2("No more staked token outputs to choose from");
        THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_staked_token_balance(subaddr_account), needed_staked_tokens, accumulated_cash_fee + needed_fee);
      }

      if ((dsts[0].output_type == tx_out_type::out_safex_account)) {
        //safex account is created from create command referencing token output, but does not directly references tokens locked for its creation (there is separate locked token output)

        LOG_PRINT_L2("Adding advanced output" << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
                                         " with blobdata: " << dsts[0].output_data);

        tx.add(dsts[0].addr, dsts[0].is_subaddress, dsts[0].output_type, dsts[0].amount, dsts[0].token_amount, original_output_index, false, dsts[0].output_data);

        pop_index(dsts, 0);
        ++original_output_index;
        if (dsts[0].output_type == tx_out_type::out_safex_account) continue; //no need to match any index
      }

      // get a random unspent cash, token or advanced output and use it to pay part (or all) of the current destination (and maybe next one, etc)
      // This could be more clever, but maybe at the cost of making probabilistic inferences easier
      size_t idx;
      if ((dsts.empty() || ((needed_tokens > 0 && dsts[0].token_amount == 0) || (needed_cash > 0 && dsts[0].amount == 0) || (needed_staked_tokens > 0 && dsts[0].token_amount == 0)))
          && !adding_fee && !advanced_output_reference)
      {
        std::vector<size_t> indices;

        if (dsts[0].output_type == tx_out_type::out_staked_token)
          idx = pop_ideal_value(indices, tx.selected_transfers, dsts[0].output_type, dsts[0].amount, dsts[0].token_amount);
        else
          idx = pop_best_value(indices, tx.selected_transfers, true, dsts[0].output_type);

        // we might not want to add it if it's a large output and we don't have many left
        if (needed_staked_tokens > 0 && m_transfers[idx].token_amount() >= m_min_output_value && m_transfers[idx].get_out_type() == tx_out_type::out_staked_token)
        {
          if (get_count_above(m_transfers, *unused_staked_token_transfers_indices, m_min_output_value) < m_min_output_count)
          {
            LOG_PRINT_L2("Second staked token output was not strictly needed, and we're running out of outputs above " << print_money(m_min_output_value) << ", not adding");
            break;
          }
        }

        // we might not want to add it if it's a large output and we don't have many left
        if (needed_tokens > 0 && m_transfers[idx].token_amount() >= m_min_output_value && m_transfers[idx].get_out_type() == tx_out_type::out_token)
        {
          if (get_count_above(m_transfers, *unused_token_transfers_indices, m_min_output_value) < m_min_output_count)
          {
            LOG_PRINT_L2("Second token output was not strictly needed, and we're running out of outputs above " << print_money(m_min_output_value) << ", not adding");
            break;
          }
        }

        if (needed_cash > 0 && m_transfers[idx].amount() >= m_min_output_value && m_transfers[idx].get_out_type() == tx_out_type::out_cash)
        {
          if (get_count_above(m_transfers, *unused_cash_transfers_indices, m_min_output_value) <
              m_min_output_count)
          {
            LOG_PRINT_L2("Second token output was not strictly needed, and we're running out of outputs above "
                                 << print_money(m_min_output_value) << ", not adding");
            break;
          }
        }

        // since we're trying to add a second output which is not strictly needed,
        // we only add it if it's unrelated enough to the first one
        float relatedness = get_output_relatedness(m_transfers[idx], m_transfers[tx.selected_transfers.front()]);
        if (relatedness > SECOND_OUTPUT_RELATEDNESS_THRESHOLD)
        {
          LOG_PRINT_L2("Second output was not strictly needed, and relatedness " << relatedness << ", not adding");
          break;
        }

        if (needed_staked_tokens > 0)
        {
          pop_if_present(*unused_staked_token_transfers_indices, idx);
        }
        else if (needed_tokens > 0)
        {
          pop_if_present(*unused_token_transfers_indices, idx);
        }
        else if (needed_cash > 0)
        {
          pop_if_present(*unused_cash_transfers_indices, idx);
          pop_if_present(*unused_cash_dust_indices, idx);
        }
        else if (dsts[0].output_type == tx_out_type::out_safex_account_update) {
          safex::edit_account_data account;
          cryptonote::parse_and_validate_from_blob(dsts[0].output_data, account);
          //find account output
          idx = pop_advanced_output(tx.selected_transfers, account.username, tx_out_type::out_safex_account);


        }
      }
      else if (adding_fee)
      {
        idx = pop_best_value(unused_cash_transfers_indices->empty() ? *unused_cash_dust_indices : *unused_cash_transfers_indices, tx.selected_transfers, false, tx_out_type::out_cash);
      }
      else
      {
        if (needed_staked_tokens > 0)
        {
          idx = pop_ideal_value(*unused_staked_token_transfers_indices, tx.selected_transfers, tx_out_type::out_staked_token, 0, needed_staked_tokens);
        }
        else if (needed_tokens > 0)
        {
          idx = pop_best_value(*unused_token_transfers_indices, tx.selected_transfers, true, tx_out_type::out_token);
        }
        else if (needed_cash > 0)
        {
          idx = pop_best_value(unused_cash_transfers_indices->empty() ? *unused_cash_dust_indices : *unused_cash_transfers_indices, tx.selected_transfers, true, tx_out_type::out_cash);
        }
        else if (dsts[0].output_type == tx_out_type::out_safex_account_update) {
          safex::edit_account_data account;
          cryptonote::parse_and_validate_from_blob(dsts[0].output_data, account);
          //find account output
          idx = pop_advanced_output(tx.selected_transfers, account.username, tx_out_type::out_safex_account);


        }


      }


      const transfer_details &td = m_transfers[idx];
      if (adding_fee || needed_cash > 0)
        LOG_PRINT_L2("Picking output " << idx << ", cash amount " << print_money(td.amount()) << ", ki " << td.m_key_image);
      else if (needed_tokens > 0)
        LOG_PRINT_L2("Picking output " << idx << ", token amount " << print_money(td.token_amount()) << ", ki " << td.m_key_image);
      else if (needed_staked_tokens > 0)
        LOG_PRINT_L2("Picking output " << idx << ", staked token amount " << print_money(td.token_amount()) << ", ki " << td.m_key_image);

      // add this output to the list to spend
      uint64_t available_cash_amount = td.amount();
      uint64_t available_token_amount = td.get_out_type() == tx_out_type::out_token ? td.token_amount() : 0;
      uint64_t available_staked_token_amount = td.get_out_type() == tx_out_type::out_staked_token ? td.token_amount() : 0;

      tx.selected_transfers.push_back(idx);
      accumulated_cash_outputs += available_cash_amount;
      accumulated_token_outputs += available_token_amount;
      accumulated_staked_token_outputs += available_staked_token_amount;

      // clear any fake outs we'd already gathered, since we'll need a new set
      outs.clear();

      const size_t additional_distribute_inputs = (command_type == safex::command_t::token_unstake) ? 1 : 0; //count into estimation safex network fee distribution inputs
      const size_t additional_distribute_outputs = (command_type == safex::command_t::token_unstake) ? 1 : 0; //count into estimation safex network fee distribution outputs

      if (adding_fee)
      {
        LOG_PRINT_L2("We need more fee, adding it to fee");
        available_for_fee += available_cash_amount;
      }
      else
      {
        while (!dsts.empty() && (((dsts[0].token_amount <= available_token_amount) || ( command_type == safex::command_t::token_unstake && dsts[0].token_amount <= available_staked_token_amount))
        && (dsts[0].amount <= available_cash_amount)) &&
               estimate_tx_size(tx.selected_transfers.size() + additional_distribute_inputs, fake_outs_count, tx.dsts.size() + additional_distribute_outputs, extra.size()) < TX_SIZE_TARGET(upper_transaction_size_limit))
        {
          // we can fully pay that destination
          LOG_PRINT_L2("We can fully pay " << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
                                           " for " << print_money(dsts[0].amount)<<" cash " << print_money(dsts[0].output_type == tx_out_type::out_token && dsts[0].token_amount)<<" tokens"
                                           << print_money(dsts[0].output_type == tx_out_type::out_staked_token &&  dsts[0].token_amount)<<" staked tokens");

          tx.add(dsts[0].addr, dsts[0].is_subaddress, dsts[0].output_type, dsts[0].amount, dsts[0].token_amount, original_output_index, m_merge_destinations, dsts[0].output_data);
          if (command_type == safex::command_t::token_unstake)
            available_staked_token_amount -=  dsts[0].token_amount;
          else
            available_token_amount -= dsts[0].token_amount;

          available_cash_amount -= dsts[0].amount;
          dsts[0].token_amount = 0;
          dsts[0].amount = 0;
          pop_index(dsts, 0);
          ++original_output_index;
        }

        if ((needed_staked_tokens > 0 && available_staked_token_amount > 0) && !dsts.empty()  && dsts[0].token_amount == available_staked_token_amount
        && estimate_tx_size(tx.selected_transfers.size() + additional_distribute_inputs, fake_outs_count, tx.dsts.size() + additional_distribute_outputs, extra.size()) < TX_SIZE_TARGET(upper_transaction_size_limit))
        {
          // we can partially fill that destination
          LOG_PRINT_L2("We can partially pay " << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
                                               " for " << print_money(available_staked_token_amount) << "/" << print_money(dsts[0].token_amount));
          tx.add(dsts[0].addr, dsts[0].is_subaddress, dsts[0].output_type, 0, available_staked_token_amount, original_output_index, m_merge_destinations, dsts[0].output_data);
          dsts[0].token_amount -= available_staked_token_amount;
          available_staked_token_amount = 0;
        }

        if ((needed_tokens > 0 && available_token_amount > 0) && !dsts.empty() && estimate_tx_size(tx.selected_transfers.size()+additional_distribute_inputs, fake_outs_count,
                tx.dsts.size() + additional_distribute_outputs, extra.size())
                < TX_SIZE_TARGET(upper_transaction_size_limit))
        {
          // we can partially fill that destination
          LOG_PRINT_L2("We can partially pay " << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
                                               " for " << print_money(available_token_amount) << "/" << print_money(dsts[0].token_amount));
          tx.add(dsts[0].addr, dsts[0].is_subaddress, dsts[0].output_type, 0, available_token_amount, original_output_index, m_merge_destinations, dsts[0].output_data);
          dsts[0].token_amount -= available_token_amount;
          available_token_amount = 0;
        }

        if ((needed_cash > 0 && available_cash_amount > 0) && !dsts.empty() && estimate_tx_size(tx.selected_transfers.size() + additional_distribute_inputs, fake_outs_count,
                tx.dsts.size() + additional_distribute_outputs, extra.size()) < TX_SIZE_TARGET(upper_transaction_size_limit))
        {
          // we can partially fill that destination
          LOG_PRINT_L2("We can partially pay " << get_account_address_as_str(m_nettype, dsts[0].is_subaddress, dsts[0].addr) <<
                                               " for " << print_money(available_cash_amount) << "/" << print_money(dsts[0].amount));
          tx.add(dsts[0].addr, dsts[0].is_subaddress, dsts[0].output_type, available_cash_amount, 0, original_output_index, m_merge_destinations, dsts[0].output_data);
          dsts[0].amount -= available_cash_amount;
          available_cash_amount = 0;
        }
      }

      // here, check if we need to sent tx and start a new one
      LOG_PRINT_L2("Considering whether to create a tx now, " << tx.selected_transfers.size() << " inputs, tx limit " << upper_transaction_size_limit);
      bool try_tx = false;
      // if we have preferred picks, but haven't yet used all of them, continue
      if (adding_fee)
      {
        /* might not actually be enough if adding this output bumps size to next kB, but we need to try */
        try_tx = available_for_fee >= needed_fee;
      }
      else
      {
        const size_t estimated_tx_size = estimate_tx_size(tx.selected_transfers.size() + additional_distribute_inputs, fake_outs_count,
                tx.dsts.size()+ additional_distribute_outputs, extra.size());
        try_tx = dsts.empty() || (estimated_tx_size >= TX_SIZE_TARGET(upper_transaction_size_limit));
      }


      if (try_tx)
      {
        cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
        pending_tx test_ptx = AUTO_VAL_INIT(test_ptx);

        //Now, we can calculate fee, and go back one more round to select cash
        //inputs to pay that fee
        const size_t estimated_tx_size = estimate_tx_size(tx.selected_transfers.size()+ additional_distribute_inputs, fake_outs_count,
                tx.dsts.size() + additional_distribute_outputs, extra.size());
        needed_fee = calculate_fee(fee_per_kb, estimated_tx_size, fee_multiplier);

        uint64_t inputs = 0, outputs = needed_fee;
        for (size_t idx: tx.selected_transfers)
        {
          inputs += m_transfers[idx].amount();
        }
        for (const auto &o: tx.dsts)
        {
          outputs += o.amount;
        }

        if (inputs == 0 || inputs < outputs)
        {
          LOG_PRINT_L2("Switching to adding_fee mode for advanced transaction");
          adding_fee = true;
          goto skip_tx;
        }

        LOG_PRINT_L2("Trying to create a tx now, with " << tx.dsts.size() << " outputs and " <<
                                                        tx.selected_transfers.size() << " inputs");
        transfer_advanced<detail::split_strategy_function>(command_type, tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time,
                needed_fee, extra, detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx, sfx_acc);

        auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
        needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
        available_for_fee = test_ptx.fee + test_ptx.change_dts.amount + (!test_ptx.dust_added_to_fee ? test_ptx.dust : 0);
        LOG_PRINT_L2("Made a " << get_size_string(txBlob) << " tx, with " << print_money(available_for_fee) << " available for fee (" <<
                               print_money(needed_fee) << " needed)");

        if (needed_fee > available_for_fee && !dsts.empty() && dsts[0].amount > 0)
        {
          // we don't have enough for the fee, but we've only partially paid the current address,
          // so we can take the fee from the paid amount, since we'll have to make another tx anyway
          std::vector<cryptonote::tx_destination_entry>::iterator i;
          i = std::find_if(tx.dsts.begin(), tx.dsts.end(),
                           [&](const cryptonote::tx_destination_entry &d)
                           { return !memcmp(&d.addr, &dsts[0].addr, sizeof(dsts[0].addr)); });
          THROW_WALLET_EXCEPTION_IF(i == tx.dsts.end(), error::wallet_internal_error, "paid address not found in outputs");
          if (i->amount > needed_fee)
          {
            uint64_t new_paid_amount = i->amount /*+ test_ptx.fee*/ - needed_fee;
            LOG_PRINT_L2("Adjusting amount paid to " << get_account_address_as_str(m_nettype, i->is_subaddress, i->addr) << " from " <<
                                                     print_money(i->amount) << " to " << print_money(new_paid_amount) << " to accommodate " <<
                                                     print_money(needed_fee) << " fee");
            dsts[0].amount += i->amount - new_paid_amount;
            i->amount = new_paid_amount;
            test_ptx.fee = needed_fee;
            available_for_fee = needed_fee;
          }
        }

        if (needed_fee > available_for_fee)
        {
          LOG_PRINT_L2("We could not make a tx, switching to fee accumulation");

          adding_fee = true;
        }
        else
        {
          LOG_PRINT_L2("We made a tx, adjusting fee and saving it, we need " << print_money(needed_fee) << " and we have " << print_money(test_ptx.fee));
          while (needed_fee > test_ptx.fee)
          {
            transfer_advanced(command_type, tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_fee, extra,
                              detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx, sfx_acc);
            txBlob = t_serializable_object_to_blob(test_ptx.tx);
            needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
            LOG_PRINT_L2("Made an attempt at a  final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
                                                        " fee  and " << print_money(test_ptx.change_dts.amount) << " cash change and" << print_money(test_ptx.change_token_dts.token_amount) << " token change");
          }

          LOG_PRINT_L2("Made a final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
                                       " fee  and " << print_money(test_ptx.change_dts.amount) << " cash change and" << print_money(test_ptx.change_token_dts.token_amount) << " token change");

          tx.tx = test_tx;
          tx.ptx = test_ptx;
          tx.bytes = txBlob.size();
          tx.outs = outs;
          accumulated_cash_fee += test_ptx.fee;
          accumulated_cash_change += test_ptx.change_dts.amount;
          accumulated_token_change += test_ptx.change_token_dts.token_amount;
          adding_fee = false;
          if (!dsts.empty())
          {
            LOG_PRINT_L2("We have more to pay, starting another tx");
            txes.push_back(ADVANCED_TX());
            original_output_index = 0;
            needed_fee = 0;
          }
        }
      }

      skip_tx:
      // if unused_*_indices is empty while unused_*_indices_per_subaddr has multiple elements, and if we still have something to pay,
      // pop front of unused_*_indices_per_subaddr and have unused_*_indices point to the front of unused_*_indices_per_subaddr
      if (!dsts.empty() && (dsts[0].token_amount > 0) && (!adding_fee))
      {
        if (command_type == safex::command_t::token_unstake)
        {
          if (unused_staked_token_transfers_indices->empty() && unused_staked_token_transfers_indices_per_subaddr.size() > 1)
          {
            unused_staked_token_transfers_indices_per_subaddr.erase(unused_staked_token_transfers_indices_per_subaddr.begin());
            unused_staked_token_transfers_indices = &unused_staked_token_transfers_indices_per_subaddr[0].second;
          }
        }
        else
        {
          if (unused_token_transfers_indices->empty() && unused_token_transfers_indices_per_subaddr.size() > 1)
          {
            unused_token_transfers_indices_per_subaddr.erase(unused_token_transfers_indices_per_subaddr.begin());
            unused_token_transfers_indices = &unused_token_transfers_indices_per_subaddr[0].second;
          }
        }
      }

      //Cash indices, for fee
      if ((dsts[0].amount > 0) || (adding_fee))
      {
        if (unused_cash_transfers_indices->empty() && unused_cash_transfers_indices_per_subaddr.size() > 1)
        {
          unused_cash_transfers_indices_per_subaddr.erase(unused_cash_transfers_indices_per_subaddr.begin());
          unused_cash_transfers_indices = &unused_cash_transfers_indices_per_subaddr[0].second;
        }
        if (unused_cash_dust_indices->empty() && unused_cash_dust_indices_per_subaddr.size() > 1)
        {
          unused_cash_dust_indices_per_subaddr.erase(unused_cash_dust_indices_per_subaddr.begin());
          unused_cash_dust_indices = &unused_cash_dust_indices_per_subaddr[0].second;
        }
      }
    }

    if (adding_fee)
    {
      LOG_PRINT_L1("We ran out of outputs while trying to gather final fee");
      THROW_WALLET_EXCEPTION_IF(1, error::tx_not_possible, unlocked_balance(subaddr_account), needed_cash, accumulated_cash_fee + needed_fee);
    }

    LOG_PRINT_L1("Done creating " << txes.size() << " transactions, " << print_money(accumulated_cash_fee) <<
                                  " total fee, " << print_money(accumulated_cash_change) << " total cash change and " << print_money(accumulated_token_change) << " total token change");

    hwdev.set_mode(hw::device::TRANSACTION_CREATE_REAL);
    for (std::vector<ADVANCED_TX>::iterator i = txes.begin(); i != txes.end(); ++i)
    {
      ADVANCED_TX &tx = *i;
      cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
      pending_tx test_ptx = AUTO_VAL_INIT(test_ptx);

      transfer_advanced(command_type,
                        tx.dsts,
                        tx.selected_transfers,
                        fake_outs_count,
                        tx.outs,
                        unlock_time,
                        tx.ptx.fee,
                        extra,
                        detail::digit_split_strategy,
                        tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD),
                        test_tx,
                        test_ptx,
                        sfx_acc);

      auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
      tx.tx = test_tx;
      tx.ptx = test_ptx;
      tx.bytes = txBlob.size();
    }

    std::vector<wallet::pending_tx> ptx_vector;
    for (std::vector<ADVANCED_TX>::iterator i = txes.begin(); i != txes.end(); ++i)
    {
      ADVANCED_TX &tx = *i;
      uint64_t tx_money = 0;
      uint64_t tx_tokens = 0;
      for (size_t idx: tx.selected_transfers)
        tx_money += m_transfers[idx].amount();
      for (size_t idx: tx.selected_transfers)
        tx_tokens += m_transfers[idx].token_amount();
      LOG_PRINT_L1("  Transaction " << (1 + std::distance(txes.begin(), i)) << "/" << txes.size() <<
                                    ": " << get_size_string(tx.bytes) << ", sending " << print_money(tx_tokens) << " tokens and" ", sending " << print_money(tx_money) << " cash in " << tx.selected_transfers.size() <<
                                    " outputs to " << tx.dsts.size() << " destination(s), including " <<
                                    print_money(tx.ptx.fee) << " fee, " << print_money(tx.ptx.change_token_dts.token_amount) << " token change and " << print_money(tx.ptx.change_dts.amount) << " cash change");
      ptx_vector.push_back(tx.ptx);
    }

    // if we made it this far, we're OK to actually send the transactions
    return ptx_vector;
  }


std::vector<wallet::pending_tx> wallet::create_transactions_from(const cryptonote::account_public_address &address, bool is_subaddress, std::vector<size_t> unused_transfers_indices, std::vector<size_t> unused_dust_indices, const size_t fake_outs_count, const uint64_t unlock_time, uint32_t priority, const std::vector<uint8_t>& extra, bool trusted_daemon)
{
  //ensure device is let in NONE mode in any case
  hw::device &hwdev = m_account.get_device();
  boost::unique_lock<hw::device> hwdev_lock (hwdev);
  hw::reset_mode rst(hwdev);

  uint64_t accumulated_fee, accumulated_outputs, accumulated_change;
  struct TX {
    std::vector<size_t> selected_transfers;
    std::vector<cryptonote::tx_destination_entry> dsts;
    cryptonote::transaction tx;
    pending_tx ptx;
    size_t bytes;
    std::vector<std::vector<get_outs_entry>> outs;
  };
  std::vector<TX> txes;
  uint64_t needed_fee, available_for_fee = 0;
  uint64_t upper_transaction_size_limit = get_upper_transaction_size_limit();
  std::vector<std::vector<get_outs_entry>> outs;

  const uint64_t fee_per_kb  = get_per_kb_fee();
  const uint64_t fee_multiplier = get_fee_multiplier(priority, get_fee_algorithm());

  LOG_PRINT_L2("Starting with " << unused_transfers_indices.size() << " non-dust outputs and " << unused_dust_indices.size() << " dust outputs");

  if (unused_dust_indices.empty() && unused_transfers_indices.empty())
    return std::vector<wallet::pending_tx>();

  // start with an empty tx
  txes.push_back(TX());
  accumulated_fee = 0;
  accumulated_outputs = 0;
  accumulated_change = 0;
  needed_fee = 0;

  // while we have something to send
  hwdev.set_mode(hw::device::TRANSACTION_CREATE_FAKE);
  while (!unused_dust_indices.empty() || !unused_transfers_indices.empty()) {
    TX &tx = txes.back();

    // get a random unspent output and use it to pay next chunk. We try to alternate
    // dust and non dust to ensure we never get with only dust, from which we might
    // get a tx that can't pay for itself
    size_t idx = unused_transfers_indices.empty() ? pop_best_value(unused_dust_indices, tx.selected_transfers) : unused_dust_indices.empty() ? pop_best_value(unused_transfers_indices, tx.selected_transfers) : ((tx.selected_transfers.size() & 1) || accumulated_outputs > fee_per_kb * fee_multiplier * (upper_transaction_size_limit + 1023) / 1024) ? pop_best_value(unused_dust_indices, tx.selected_transfers) : pop_best_value(unused_transfers_indices, tx.selected_transfers);

    const transfer_details &td = m_transfers[idx];
    LOG_PRINT_L2("Picking output " << idx << ", amount " << print_money(td.amount()));

    // add this output to the list to spend
    tx.selected_transfers.push_back(idx);
    uint64_t available_amount = td.amount();
    accumulated_outputs += available_amount;

    // clear any fake outs we'd already gathered, since we'll need a new set
    outs.clear();

    // here, check if we need to sent tx and start a new one
    LOG_PRINT_L2("Considering whether to create a tx now, " << tx.selected_transfers.size() << " inputs, tx limit " << upper_transaction_size_limit);
    const size_t estimated_tmp_tx_size = estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size() + 1, extra.size());
    bool try_tx = (unused_dust_indices.empty() && unused_transfers_indices.empty()) || ( estimated_tmp_tx_size >= TX_SIZE_TARGET(upper_transaction_size_limit));

    if (try_tx) {
      cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
      pending_tx test_ptx;

      const size_t estimated_tx_size = estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size());
      needed_fee = calculate_fee(fee_per_kb, estimated_tx_size, fee_multiplier);

      tx.dsts.push_back(tx_destination_entry(1, address, is_subaddress));

      LOG_PRINT_L2("Trying to create a tx now, with " << tx.dsts.size() << " destinations and " <<
        tx.selected_transfers.size() << " outputs");

      transfer_selected(tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_fee, extra,
          detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);

      auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
      needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
      available_for_fee = test_ptx.fee + test_ptx.dests[0].amount + test_ptx.change_dts.amount;
      LOG_PRINT_L2("Made a " << get_size_string(txBlob) << " tx, with " << print_money(available_for_fee) << " available for fee (" <<
        print_money(needed_fee) << " needed)");

      THROW_WALLET_EXCEPTION_IF(needed_fee > available_for_fee, error::wallet_internal_error, "Transaction cannot pay for itself");

      do {
        LOG_PRINT_L2("We made a tx, adjusting fee and saving it");
        tx.dsts[0].amount = available_for_fee - needed_fee;

        transfer_selected(tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_fee, extra,
            detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);

        txBlob = t_serializable_object_to_blob(test_ptx.tx);
        needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
        LOG_PRINT_L2("Made an attempt at a final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
          " fee  and " << print_money(test_ptx.change_dts.amount) << " change");
      } while (needed_fee > test_ptx.fee);

      LOG_PRINT_L2("Made a final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
        " fee  and " << print_money(test_ptx.change_dts.amount) << " change");

      tx.tx = test_tx;
      tx.ptx = test_ptx;
      tx.bytes = txBlob.size();
      tx.outs = outs;
      accumulated_fee += test_ptx.fee;
      accumulated_change += test_ptx.change_dts.amount;
      if (!unused_transfers_indices.empty() || !unused_dust_indices.empty())
      {
        LOG_PRINT_L2("We have more to pay, starting another tx");
        txes.push_back(TX());
      }
    }
  }

  LOG_PRINT_L1("Done creating " << txes.size() << " transactions, " << print_money(accumulated_fee) <<
    " total fee, " << print_money(accumulated_change) << " total change");

  hwdev.set_mode(hw::device::TRANSACTION_CREATE_REAL);
  for (std::vector<TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TX &tx = *i;
    cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
    pending_tx test_ptx;

    transfer_selected(tx.dsts, tx.selected_transfers, fake_outs_count, tx.outs, unlock_time, needed_fee, extra,
        detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);

    auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
    tx.tx = test_tx;
    tx.ptx = test_ptx;
    tx.bytes = txBlob.size();
  }

  std::vector<wallet::pending_tx> ptx_vector;
  for (std::vector<TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TX &tx = *i;
    uint64_t tx_money = 0;
    for (size_t idx: tx.selected_transfers)
      tx_money += m_transfers[idx].amount();
    LOG_PRINT_L1("  Transaction " << (1+std::distance(txes.begin(), i)) << "/" << txes.size() <<
      ": " << get_size_string(tx.bytes) << ", sending " << print_money(tx_money) << " in " << tx.selected_transfers.size() <<
      " outputs to " << tx.dsts.size() << " destination(s), including " <<
      print_money(tx.ptx.fee) << " fee, " << print_money(tx.ptx.change_dts.amount) << " change");
    ptx_vector.push_back(tx.ptx);
  }

  // if we made it this far, we're OK to actually send the transactions
  return ptx_vector;
}
//----------------------------------------------------------------------------------------------------
std::vector<wallet::pending_tx> wallet::create_transactions_token_from(const cryptonote::account_public_address &address, bool is_subaddress, std::vector<size_t> unused_token_transfers_indices, std::vector<size_t> unused_token_dust_indices,
                                                                       std::vector<size_t> unused_transfers_indices, std::vector<size_t> unused_dust_indices, const size_t fake_outs_count, const uint64_t unlock_time, uint32_t priority,
                                                                       const std::vector<uint8_t> &extra, bool trusted_daemon)
{
  //ensure device is let in NONE mode in any case
  hw::device &hwdev = m_account.get_device();
  boost::unique_lock<hw::device> hwdev_lock(hwdev);
  hw::reset_mode rst(hwdev);

  struct TOKEN_TX
  {
    std::vector<size_t> selected_transfers;
    std::vector<cryptonote::tx_destination_entry> dsts;
    cryptonote::transaction tx;
    pending_tx ptx = AUTO_VAL_INIT(ptx);
    size_t bytes = 0;
    std::vector<std::vector<get_outs_entry>> outs;
  };
  uint64_t upper_transaction_size_limit = get_upper_transaction_size_limit();
  std::vector<std::vector<get_outs_entry>> outs;

  const uint64_t fee_per_kb = get_per_kb_fee();
  const uint64_t fee_multiplier = get_fee_multiplier(priority, get_fee_algorithm());

  LOG_PRINT_L2("Starting with " << unused_token_transfers_indices.size() << " non-dust outputs and " << unused_token_dust_indices.size() << " dust outputs");

  if (unused_token_dust_indices.empty() && unused_token_transfers_indices.empty())
    return std::vector<wallet::pending_tx>();

  if (unused_dust_indices.empty() && unused_transfers_indices.empty())
    return std::vector<wallet::pending_tx>();

  // start with an empty tx
  std::vector<TOKEN_TX> txes;
  txes.push_back(TOKEN_TX());

  uint64_t needed_fee = 0, available_for_fee = 0;
  uint64_t accumulated_fee = 0, accumulated_change = 0, accumulated_outputs = 0 /* cash outputs for fee*/;
  uint64_t accumulated_token_outputs = 0, accumulated_token_change = 0;
  bool adding_fee = false;

  // while we have something to send
  hwdev.set_mode(hw::device::TRANSACTION_CREATE_FAKE);
  while (!unused_token_dust_indices.empty() || !unused_token_transfers_indices.empty() || adding_fee)
  {
    TOKEN_TX &tx = txes.back();

    LOG_PRINT_L2("Start of loop with " << unused_token_transfers_indices.size() << " " << unused_token_dust_indices.size());
    LOG_PRINT_L2("unused_token_transfers_indices: " << (unused_token_transfers_indices.size() < 100 ? strjoin(unused_token_transfers_indices, " ") : " too big to print"));
    LOG_PRINT_L2("unused_token_dust_indices: " << (unused_token_dust_indices.size() < 100 ? strjoin(unused_token_dust_indices, " ") : " too big to print"));
    LOG_PRINT_L2("unused_transfers_indices: " << (unused_transfers_indices.size()?strjoin(unused_transfers_indices, " "):"too big to print"));
    LOG_PRINT_L2("unused_dust_indices: " << (unused_dust_indices.size() ? strjoin(unused_dust_indices, " ") : " "));
    LOG_PRINT_L2("dsts size " << tx.dsts.size() << ", first token " << (tx.dsts.empty() ? "-" : cryptonote::print_money(tx.dsts[0].token_amount)));
    LOG_PRINT_L2("adding_fee " << adding_fee);


    size_t idx;

    if (adding_fee)
    {
      //get cash output for fee;
      idx = unused_transfers_indices.empty() ? pop_best_value(unused_dust_indices, tx.selected_transfers, false, tx_out_type::out_cash) : unused_dust_indices.empty() ? pop_best_value(unused_transfers_indices, tx.selected_transfers, false, tx_out_type::out_cash) :
              ((tx.selected_transfers.size() & 1) || accumulated_outputs > fee_per_kb * fee_multiplier * (upper_transaction_size_limit + 1023) / 1024) ? pop_best_value(unused_dust_indices, tx.selected_transfers, false, tx_out_type::out_cash) :
              pop_best_value(unused_transfers_indices, tx.selected_transfers, false, tx_out_type::out_cash);

    } else
    {
      // get a random unspent output and use it to pay next chunk. We try to alternate
      // dust and non dust to ensure we never get with only dust, from which we might
      // get a tx that can't pay for itself
      idx = unused_token_transfers_indices.empty() ? pop_best_value(unused_token_dust_indices, tx.selected_transfers, false, tx_out_type::out_token) :
            unused_token_dust_indices.empty() ? pop_best_value(unused_token_transfers_indices, tx.selected_transfers, true, tx_out_type::out_token) :
            (tx.selected_transfers.size() & 1) ? pop_best_value(unused_token_dust_indices, tx.selected_transfers, false, tx_out_type::out_token) : pop_best_value(unused_token_transfers_indices, tx.selected_transfers, true, tx_out_type::out_token);
    }

    const transfer_details &td = m_transfers[idx];
    LOG_PRINT_L2("Picking output " << idx << ", token amount " << print_money(td.token_amount()) << " cash amount " << print_money(td.amount()));

    // add this output to the list to spend
    tx.selected_transfers.push_back(idx);
    uint64_t available_token_amount = td.token_amount();
    uint64_t available_amount = td.amount();
    accumulated_token_outputs += available_token_amount;
    accumulated_outputs += available_amount;

    // clear any fake outs we'd already gathered, since we'll need a new set
    outs.clear();

    // here, check if we need to sent tx and start a new one
    LOG_PRINT_L2("Considering whether to create a tx now, " << tx.selected_transfers.size() << " inputs, tx limit " << upper_transaction_size_limit);
    const size_t estimated_rct_tx_size = estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size() + 1, extra.size());
    bool try_tx = (unused_token_dust_indices.empty() && unused_token_transfers_indices.empty()) || (estimated_rct_tx_size >= TX_SIZE_TARGET(upper_transaction_size_limit)) || adding_fee;

    if (try_tx)
    {
      cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
      pending_tx test_ptx = AUTO_VAL_INIT(test_ptx);

      const size_t estimated_tx_size = estimate_tx_size(tx.selected_transfers.size(), fake_outs_count, tx.dsts.size(), extra.size());
      needed_fee = calculate_fee(fee_per_kb, estimated_tx_size, fee_multiplier);

      if (needed_fee > 0 && accumulated_fee == 0 && accumulated_outputs == 0)
      {
        adding_fee = true;
        continue;
      }

      if (adding_fee)
        tx.dsts.push_back(tx_destination_entry(1, address, is_subaddress));

      LOG_PRINT_L2("Trying to create a tx now, with " << tx.dsts.size() << " destinations and " << tx.selected_transfers.size() << " outputs");

      transfer_selected<detail::split_strategy_function, cryptonote::tx_out_type::out_token>(tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_fee, extra,
                                                                                               detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);
      auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
      needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
      available_for_fee = test_ptx.fee + test_ptx.dests[0].amount + test_ptx.change_dts.amount;
      LOG_PRINT_L2("Made a " << get_size_string(txBlob) << " token tx, with " << print_money(available_for_fee) << " available for fee (" << print_money(needed_fee) << " needed)");

      THROW_WALLET_EXCEPTION_IF(needed_fee > available_for_fee, error::wallet_internal_error, "Transaction cannot pay for itself");

      do
      {
        LOG_PRINT_L2("We made a tx, adjusting fee and saving it");
        tx.dsts[0].amount = available_for_fee - needed_fee;

        transfer_selected<detail::split_strategy_function, cryptonote::tx_out_type::out_token>(tx.dsts, tx.selected_transfers, fake_outs_count, outs, unlock_time, needed_fee, extra,
                                                                                                 detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);
        txBlob = t_serializable_object_to_blob(test_ptx.tx);
        needed_fee = calculate_fee(fee_per_kb, txBlob, fee_multiplier);
        LOG_PRINT_L2("Made an attempt at a final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
                                                   " fee  and " << print_money(test_ptx.change_token_dts.token_amount) << " token change and " << print_money(test_ptx.change_dts.amount) << " change");
      } while (needed_fee > test_ptx.fee);

      LOG_PRINT_L2("Made a final " << get_size_string(txBlob) << " tx, with " << print_money(test_ptx.fee) <<
                                   " fee  and " << print_money(test_ptx.change_token_dts.token_amount) << " token change and " << print_money(test_ptx.change_dts.amount) << " change");

      tx.tx = test_tx;
      tx.ptx = test_ptx;
      tx.bytes = txBlob.size();
      tx.outs = outs;
      accumulated_fee += test_ptx.fee;
      accumulated_token_change += test_ptx.change_token_dts.token_amount;
      accumulated_change += test_ptx.change_dts.amount;
      if ((!unused_token_transfers_indices.empty() || !unused_token_dust_indices.empty()) && (!unused_transfers_indices.empty()))
      {
        LOG_PRINT_L2("We have more to pay, starting another token tx");
        txes.push_back(TOKEN_TX());
        needed_fee = 0;
      }
      adding_fee = false;
    }
  }

  LOG_PRINT_L1("Done creating " << txes.size() << " transactions, " << print_money(accumulated_fee) <<
                                " total fee, " << print_money(accumulated_token_change) << " total token change and " << print_money(accumulated_change) << " total cash change");

  hwdev.set_mode(hw::device::TRANSACTION_CREATE_REAL);
  for (std::vector<TOKEN_TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TOKEN_TX &tx = *i;
    cryptonote::transaction test_tx = AUTO_VAL_INIT(test_tx);
    pending_tx test_ptx = AUTO_VAL_INIT(test_ptx);

    transfer_selected<detail::split_strategy_function, cryptonote::tx_out_type::out_token>(tx.dsts, tx.selected_transfers, fake_outs_count, tx.outs, unlock_time, tx.ptx.fee, extra,
                                                                                             detail::digit_split_strategy, tx_dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD), test_tx, test_ptx);

    auto txBlob = t_serializable_object_to_blob(test_ptx.tx);
    tx.tx = test_tx;
    tx.ptx = test_ptx;
    tx.bytes = txBlob.size();
  }

  std::vector<wallet::pending_tx> ptx_vector;
  for (std::vector<TOKEN_TX>::iterator i = txes.begin(); i != txes.end(); ++i)
  {
    TOKEN_TX &tx = *i;
    uint64_t tx_money = 0;
    uint64_t tx_tokens = 0;
    for (size_t idx: tx.selected_transfers)
    {
      tx_money += m_transfers[idx].amount();
      tx_tokens += m_transfers[idx].token_amount();
    }
    LOG_PRINT_L1("  Transaction " << (1 + std::distance(txes.begin(), i)) << "/" << txes.size() <<
                                  ": " << get_size_string(tx.bytes) << ", sending " << print_money(tx_tokens) << " tokens and " <<
                                  print_money(tx_money) << " cash in " << tx.selected_transfers.size() <<
                                  " outputs to " << tx.dsts.size() << " destination(s), including " <<
                                  print_money(tx.ptx.fee) << " fee, " << print_money(tx.ptx.change_token_dts.token_amount) << " token change and " << print_money(tx.ptx.change_dts.amount) << " cash change");
    ptx_vector.push_back(tx.ptx);
  }

  // if we made it this far, we're OK to actually send the transactions
  return ptx_vector;
}
//----------------------------------------------------------------------------------------------------
void wallet::get_hard_fork_info(uint8_t version, uint64_t &earliest_height) const
{
  boost::optional<std::string> result = m_node_rpc_proxy.get_earliest_height(version, earliest_height);
  throw_on_rpc_response_error(result, "get_hard_fork_info");
}
//----------------------------------------------------------------------------------------------------
bool wallet::use_fork_rules(uint8_t version, int64_t early_blocks) const
{
  // TODO: How to get fork rule info from light wallet node?
  if(m_light_wallet)
    return true;
  uint64_t height, earliest_height;
  boost::optional<std::string> result = m_node_rpc_proxy.get_height(height);
  throw_on_rpc_response_error(result, "get_info");
  result = m_node_rpc_proxy.get_earliest_height(version, earliest_height);
  throw_on_rpc_response_error(result, "get_hard_fork_info");

  bool close_enough = height >=  earliest_height - early_blocks && earliest_height != std::numeric_limits<uint64_t>::max(); // start using the rules that many blocks beforehand
  if (close_enough)
    LOG_PRINT_L2("Using v" << (unsigned)version << " rules");
  else
    LOG_PRINT_L2("Not using v" << (unsigned)version << " rules");
  return close_enough;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::get_upper_transaction_size_limit() const
{
  if (m_upper_transaction_size_limit > 0)
    return m_upper_transaction_size_limit;
  uint64_t full_reward_zone = use_fork_rules(2,10) ? CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE_V2 : CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE_V1;
  return full_reward_zone - CRYPTONOTE_COINBASE_BLOB_RESERVED_SIZE;
}
//----------------------------------------------------------------------------------------------------
std::vector<size_t> wallet::select_available_outputs(const std::function<bool(const transfer_details &td)> &f) const
{
  std::vector<size_t> outputs;
  size_t n = 0;
  for (transfer_container::const_iterator i = m_transfers.begin(); i != m_transfers.end(); ++i, ++n)
  {
    if (i->m_spent)
      continue;
    if (i->m_key_image_partial)
      continue;
    if (!is_transfer_unlocked(*i))
      continue;
    if (f(*i))
      outputs.push_back(n);
  }
  return outputs;
}
//----------------------------------------------------------------------------------------------------
std::vector<uint64_t> wallet::get_unspent_amounts_vector() const
{
  std::set<uint64_t> set;
  for (const auto &td: m_transfers)
  {
    if (!td.m_spent)
      set.insert(td.amount());
  }
  std::vector<uint64_t> vector;
  vector.reserve(set.size());
  for (const auto &i: set)
  {
    vector.push_back(i);
  }
  return vector;
}
//----------------------------------------------------------------------------------------------------
std::vector<size_t> wallet::select_available_outputs_from_histogram(uint64_t count, bool atleast, bool unlocked, bool allow_rct, bool trusted_daemon, cryptonote::tx_out_type out_type)
{
  cryptonote::COMMAND_RPC_GET_OUTPUT_HISTOGRAM::request req_t = AUTO_VAL_INIT(req_t);
  cryptonote::COMMAND_RPC_GET_OUTPUT_HISTOGRAM::response resp_t = AUTO_VAL_INIT(resp_t);
  m_daemon_rpc_mutex.lock();
  if (trusted_daemon)
    req_t.amounts = get_unspent_amounts_vector();
  req_t.min_count = count;

  req_t.unlocked = unlocked;
  req_t.out_type = out_type;
  bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_output_histogram", req_t, resp_t, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "select_available_outputs_from_histogram");
  THROW_WALLET_EXCEPTION_IF(resp_t.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_output_histogram");
  THROW_WALLET_EXCEPTION_IF(resp_t.status != CORE_RPC_STATUS_OK, error::get_histogram_error, resp_t.status);

  std::set<uint64_t> mixable;
  for (const auto &i: resp_t.histogram)
  {
    mixable.insert(i.amount);
  }

  return select_available_outputs([mixable, atleast, allow_rct, out_type](const transfer_details &td) {
    if (!allow_rct && td.is_rct())
      return false;
    uint64_t value_amount;
    if (out_type == cryptonote::tx_out_type::out_token)
    {
      value_amount = td.token_amount();
    }
    else if (out_type == cryptonote::tx_out_type::out_cash)
    {
      value_amount = td.amount();
    }
    else
    {
      return false;
    }

    if (atleast) {
      if (mixable.find(value_amount) != mixable.end())
        return true;
    }
    else {
      if (mixable.find(value_amount) == mixable.end())
        return true;
    }
    return false;
  });
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::get_num_rct_outputs()
{
  cryptonote::COMMAND_RPC_GET_OUTPUT_HISTOGRAM::request req_t = AUTO_VAL_INIT(req_t);
  cryptonote::COMMAND_RPC_GET_OUTPUT_HISTOGRAM::response resp_t = AUTO_VAL_INIT(resp_t);
  m_daemon_rpc_mutex.lock();
  req_t.amounts.push_back(0);
  req_t.min_count = 0;
  req_t.max_count = 0;
  bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_output_histogram", req_t, resp_t, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_num_rct_outputs");
  THROW_WALLET_EXCEPTION_IF(resp_t.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_output_histogram");
  THROW_WALLET_EXCEPTION_IF(resp_t.status != CORE_RPC_STATUS_OK, error::get_histogram_error, resp_t.status);
  THROW_WALLET_EXCEPTION_IF(resp_t.histogram.size() != 1, error::get_histogram_error, "Expected exactly one response");
  THROW_WALLET_EXCEPTION_IF(resp_t.histogram[0].amount != 0, error::get_histogram_error, "Expected 0 amount");

  return resp_t.histogram[0].total_instances;
}
//----------------------------------------------------------------------------------------------------
const wallet::transfer_details &wallet::get_transfer_details(size_t idx) const
{
  THROW_WALLET_EXCEPTION_IF(idx >= m_transfers.size(), error::wallet_internal_error, "Bad transfer index");
  return m_transfers[idx];
}
//----------------------------------------------------------------------------------------------------
std::vector<size_t> wallet::select_available_unmixable_outputs(bool trusted_daemon, cryptonote::tx_out_type out_type)
{
  // request all outputs with less than 7 instances
  const size_t min_mixin = DEFAULT_MIX;
  return select_available_outputs_from_histogram(min_mixin + 1, false, true, false, trusted_daemon, out_type);
}
//----------------------------------------------------------------------------------------------------
std::vector<size_t> wallet::select_available_mixable_outputs(bool trusted_daemon, cryptonote::tx_out_type out_type)
{
  // request all outputs with at least 7 instances, so we can use mixin 6 with
  const size_t min_mixin = DEFAULT_MIX;
  return select_available_outputs_from_histogram(min_mixin + 1, true, true, true, trusted_daemon, out_type);
}
//----------------------------------------------------------------------------------------------------
std::vector<wallet::pending_tx> wallet::create_unmixable_sweep_transactions(bool trusted_daemon, cryptonote::tx_out_type out_type)
{
  //We don't consider small amounts to be dust
  tx_dust_policy dust_policy(::config::DEFAULT_DUST_THRESHOLD, ::config::DEFAULT_TOKEN_DUST_THRESHOLD);

  const uint64_t fee_per_kb = get_per_kb_fee();

  // may throw
  // collect cash outputs
  std::vector<size_t> unmixable_outputs = select_available_unmixable_outputs(trusted_daemon, cryptonote::tx_out_type::out_cash);
  size_t num_dust_outputs = unmixable_outputs.size();

  if ((num_dust_outputs == 0) && (out_type == cryptonote::tx_out_type::out_cash))
  {
    return std::vector<wallet::pending_tx>();
  }

  // split in "dust" and "non dust" to make it easier to select outputs
  std::vector<size_t> unmixable_transfer_outputs, unmixable_dust_outputs;
  for (auto n: unmixable_outputs)
  {
    if (!m_transfers[n].m_token_transfer)
    {
      if (m_transfers[n].amount() < fee_per_kb)
        unmixable_dust_outputs.push_back(n);
      else
        unmixable_transfer_outputs.push_back(n);
    }
  }

  if (out_type == cryptonote::tx_out_type::out_token)
  {
    //for tokens out sweep
    std::vector<size_t> unmixable_token_transfer_outputs, unmixable_token_dust_outputs;
    std::vector<size_t> unmixable_token_outputs = select_available_unmixable_outputs(trusted_daemon, cryptonote::tx_out_type::out_token);
    size_t num_token_dust_outputs = unmixable_token_outputs.size();

    if (num_token_dust_outputs == 0)
    {
      return std::vector<wallet::pending_tx>();
    }

    for (auto n: unmixable_token_outputs)
    {
      if (m_transfers[n].m_token_transfer)
      {
        if (m_transfers[n].token_amount() < fee_per_kb)
          unmixable_token_dust_outputs.push_back(n);
        else
          unmixable_token_transfer_outputs.push_back(n);
      }
    }

    if (num_dust_outputs == 0)
    {
      //in case of sweeping tokens, we will need some mixable cash outputs (if there are no unmixable) for fee
      std::vector<size_t> mixable_outputs;

      mixable_outputs = select_available_mixable_outputs(trusted_daemon, cryptonote::tx_out_type::out_cash);
      size_t num_nondust_outputs = mixable_outputs.size();
      if (num_nondust_outputs == 0)
      {
        //no cash outputs for fee
        return std::vector<wallet::pending_tx>();
      }

      std::vector<size_t> mixable_transfer_outputs, mixable_dust_outputs;
      for (auto n: mixable_outputs)
      {
        if (!m_transfers[n].m_token_transfer)
        {
          if (m_transfers[n].amount() < fee_per_kb)
            mixable_dust_outputs.push_back(n);
          else
            mixable_transfer_outputs.push_back(n);
        }
      }
      return create_transactions_token_from(m_account_public_address, false, unmixable_token_transfer_outputs, unmixable_token_dust_outputs,
                                            mixable_transfer_outputs, mixable_dust_outputs, 0 /*fake_outs_count */, 0 /* unlock_time */, 1 /*priority */, std::vector<uint8_t>(), trusted_daemon);
    }
    else
    {
      return create_transactions_token_from(m_account_public_address, false, unmixable_token_transfer_outputs, unmixable_token_dust_outputs,
                                            unmixable_transfer_outputs, unmixable_dust_outputs, 0 /*fake_outs_count */, 0 /* unlock_time */, 1 /*priority */, std::vector<uint8_t>(), trusted_daemon);
    }


  }
  else
  {
    return create_transactions_from(m_account_public_address, false, unmixable_transfer_outputs, unmixable_dust_outputs, 0 /*fake_outs_count */, 0 /* unlock_time */, 1 /*priority */, std::vector<uint8_t>(), trusted_daemon);
  }

}

bool wallet::get_tx_key(const crypto::hash &txid, crypto::secret_key &tx_key, std::vector<crypto::secret_key> &additional_tx_keys) const
{
  additional_tx_keys.clear();
  const std::unordered_map<crypto::hash, crypto::secret_key>::const_iterator i = m_tx_keys.find(txid);
  if (i == m_tx_keys.end())
    return false;
  tx_key = i->second;
  const auto j = m_additional_tx_keys.find(txid);
  if (j != m_additional_tx_keys.end())
    additional_tx_keys = j->second;
  return true;
}
//----------------------------------------------------------------------------------------------------
std::string wallet::get_spend_proof(const crypto::hash &txid, const std::string &message)
{
  THROW_WALLET_EXCEPTION_IF(m_watch_only, error::wallet_internal_error,
    "get_spend_proof requires spend secret key and is not available for a watch-only wallet");

  // fetch tx from daemon
  COMMAND_RPC_GET_TRANSACTIONS::request req = AUTO_VAL_INIT(req);
  req.txs_hashes.push_back(epee::string_tools::pod_to_hex(txid));
  req.decode_as_json = false;
  req.prune = false;
  COMMAND_RPC_GET_TRANSACTIONS::response res = AUTO_VAL_INIT(res);
  bool r;
  {
    const boost::lock_guard<boost::mutex> lock{m_daemon_rpc_mutex};
    r = epee::net_utils::invoke_http_json("/gettransactions", req, res, m_http_client, rpc_timeout);
  }
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "gettransactions");
  THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "gettransactions");
  THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::wallet_internal_error, "gettransactions");
  THROW_WALLET_EXCEPTION_IF(res.txs.size() != 1, error::wallet_internal_error,
    "daemon returned wrong response for gettransactions, wrong txs count = " +
    std::to_string(res.txs.size()) + ", expected 1");
  cryptonote::blobdata bd;
  THROW_WALLET_EXCEPTION_IF(!epee::string_tools::parse_hexstr_to_binbuff(res.txs[0].as_hex, bd), error::wallet_internal_error, "failed to parse tx from hexstr");
  cryptonote::transaction tx;
  crypto::hash tx_hash, tx_prefix_hash;
  THROW_WALLET_EXCEPTION_IF(!cryptonote::parse_and_validate_tx_from_blob(bd, tx, tx_hash, tx_prefix_hash), error::wallet_internal_error, "failed to parse tx from blob");
  THROW_WALLET_EXCEPTION_IF(tx_hash != txid, error::wallet_internal_error, "txid mismatch");

  std::vector<std::vector<crypto::signature>> signatures;

  // get signature prefix hash
  std::string sig_prefix_data((const char*)&txid, sizeof(crypto::hash));
  sig_prefix_data += message;
  crypto::hash sig_prefix_hash;
  crypto::cn_fast_hash(sig_prefix_data.data(), sig_prefix_data.size(), sig_prefix_hash);

  for(size_t i = 0; i < tx.vin.size(); ++i)
  {
    const txin_to_key* const in_key = boost::get<txin_to_key>(std::addressof(tx.vin[i]));
    if (in_key == nullptr)
      continue;

    // check if the key image belongs to us
    const auto found = m_key_images.find(in_key->k_image);
    if(found == m_key_images.end())
    {
      THROW_WALLET_EXCEPTION_IF(i > 0, error::wallet_internal_error, "subset of key images belong to us, very weird!");
      THROW_WALLET_EXCEPTION_IF(true, error::wallet_internal_error, "This tx wasn't generated by this wallet!");
    }

    // derive the real output keypair
    const transfer_details& in_td = m_transfers[found->second];
    const txout_to_key* const in_tx_out_pkey = boost::get<txout_to_key>(std::addressof(in_td.m_tx.vout[in_td.m_internal_output_index].target));
    THROW_WALLET_EXCEPTION_IF(in_tx_out_pkey == nullptr, error::wallet_internal_error, "Output is not txout_to_key");
    const crypto::public_key in_tx_pub_key = get_tx_pub_key_from_extra(in_td.m_tx, in_td.m_pk_index);
    const std::vector<crypto::public_key> in_additionakl_tx_pub_keys = get_additional_tx_pub_keys_from_extra(in_td.m_tx);
    keypair in_ephemeral;
    crypto::key_image in_img;
    THROW_WALLET_EXCEPTION_IF(!generate_key_image_helper(m_account.get_keys(), m_subaddresses, in_tx_out_pkey->key, in_tx_pub_key, in_additionakl_tx_pub_keys, in_td.m_internal_output_index, in_ephemeral, in_img, m_account.get_device()),
      error::wallet_internal_error, "failed to generate key image");
    THROW_WALLET_EXCEPTION_IF(in_key->k_image != in_img, error::wallet_internal_error, "key image mismatch");

    // get output pubkeys in the ring
    const std::vector<uint64_t> absolute_offsets = cryptonote::relative_output_offsets_to_absolute(in_key->key_offsets);
    const size_t ring_size = in_key->key_offsets.size();
    THROW_WALLET_EXCEPTION_IF(absolute_offsets.size() != ring_size, error::wallet_internal_error, "absolute offsets size is wrong");
    COMMAND_RPC_GET_OUTPUTS_BIN::request req = AUTO_VAL_INIT(req);
    req.outputs.resize(ring_size);
    for (size_t j = 0; j < ring_size; ++j)
    {
      req.outputs[j].amount = in_key->amount;
      req.outputs[j].index = absolute_offsets[j];
    }
    COMMAND_RPC_GET_OUTPUTS_BIN::response res = AUTO_VAL_INIT(res);
    bool r;
    {
      const boost::lock_guard<boost::mutex> lock{m_daemon_rpc_mutex};
      r = epee::net_utils::invoke_http_bin("/get_outs.bin", req, res, m_http_client, rpc_timeout);
    }
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_outs.bin");
    THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_outs.bin");
    THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::wallet_internal_error, "get_outs.bin");
    THROW_WALLET_EXCEPTION_IF(res.outs.size() != ring_size, error::wallet_internal_error,
      "daemon returned wrong response for get_outs.bin, wrong amounts count = " +
      std::to_string(res.outs.size()) + ", expected " +  std::to_string(ring_size));

    // copy pubkey pointers
    std::vector<const crypto::public_key *> p_output_keys;
    for (const COMMAND_RPC_GET_OUTPUTS_BIN::outkey &out : res.outs)
      p_output_keys.push_back(&out.key);

    // figure out real output index and secret key
    size_t sec_index = -1;
    for (size_t j = 0; j < ring_size; ++j)
    {
      if (res.outs[j].key == in_ephemeral.pub)
      {
        sec_index = j;
        break;
      }
    }
    THROW_WALLET_EXCEPTION_IF(sec_index >= ring_size, error::wallet_internal_error, "secret index not found");

    // generate ring sig for this input
    signatures.push_back(std::vector<crypto::signature>());
    std::vector<crypto::signature>& sigs = signatures.back();
    sigs.resize(in_key->key_offsets.size());
    crypto::generate_ring_signature(sig_prefix_hash, in_key->k_image, p_output_keys, in_ephemeral.sec, sec_index, sigs.data());
  }

  std::string sig_str = "SpendProofV1";
  for (const std::vector<crypto::signature>& ring_sig : signatures)
    for (const crypto::signature& sig : ring_sig)
       sig_str += tools::base58::encode(std::string((const char *)&sig, sizeof(crypto::signature)));
  return sig_str;
}
//----------------------------------------------------------------------------------------------------
bool wallet::check_spend_proof(const crypto::hash &txid, const std::string &message, const std::string &sig_str)
{
  const std::string header = "SpendProofV1";
  const size_t header_len = header.size();
  THROW_WALLET_EXCEPTION_IF(sig_str.size() < header_len || sig_str.substr(0, header_len) != header, error::wallet_internal_error,
    "Signature header check error");

  // fetch tx from daemon
  COMMAND_RPC_GET_TRANSACTIONS::request req = AUTO_VAL_INIT(req);
  req.txs_hashes.push_back(epee::string_tools::pod_to_hex(txid));
  req.decode_as_json = false;
  req.prune = false;
  COMMAND_RPC_GET_TRANSACTIONS::response res = AUTO_VAL_INIT(res);
  bool r;
  {
    const boost::lock_guard<boost::mutex> lock{m_daemon_rpc_mutex};
    r = epee::net_utils::invoke_http_json("/gettransactions", req, res, m_http_client, rpc_timeout);
  }
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "gettransactions");
  THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "gettransactions");
  THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::wallet_internal_error, "gettransactions");
  THROW_WALLET_EXCEPTION_IF(res.txs.size() != 1, error::wallet_internal_error,
    "daemon returned wrong response for gettransactions, wrong txs count = " +
    std::to_string(res.txs.size()) + ", expected 1");
  cryptonote::blobdata bd;
  THROW_WALLET_EXCEPTION_IF(!epee::string_tools::parse_hexstr_to_binbuff(res.txs[0].as_hex, bd), error::wallet_internal_error, "failed to parse tx from hexstr");
  cryptonote::transaction tx;
  crypto::hash tx_hash, tx_prefix_hash;
  THROW_WALLET_EXCEPTION_IF(!cryptonote::parse_and_validate_tx_from_blob(bd, tx, tx_hash, tx_prefix_hash), error::wallet_internal_error, "failed to parse tx from blob");
  THROW_WALLET_EXCEPTION_IF(tx_hash != txid, error::wallet_internal_error, "txid mismatch");

  // check signature size
  size_t num_sigs = 0;
  for(size_t i = 0; i < tx.vin.size(); ++i)
  {
    const txin_to_key* const in_key = boost::get<txin_to_key>(std::addressof(tx.vin[i]));
    if (in_key != nullptr)
      num_sigs += in_key->key_offsets.size();
  }
  std::vector<std::vector<crypto::signature>> signatures = { std::vector<crypto::signature>(1) };
  const size_t sig_len = tools::base58::encode(std::string((const char *)&signatures[0][0], sizeof(crypto::signature))).size();
  THROW_WALLET_EXCEPTION_IF(sig_str.size() != header_len + num_sigs * sig_len,
    error::wallet_internal_error, "incorrect signature size");

  // decode base58
  signatures.clear();
  size_t offset = header_len;
  for(size_t i = 0; i < tx.vin.size(); ++i)
  {
    const txin_to_key* const in_key = boost::get<txin_to_key>(std::addressof(tx.vin[i]));
    if (in_key == nullptr)
      continue;
    signatures.resize(signatures.size() + 1);
    signatures.back().resize(in_key->key_offsets.size());
    for (size_t j = 0; j < in_key->key_offsets.size(); ++j)
    {
      std::string sig_decoded;
      THROW_WALLET_EXCEPTION_IF(!tools::base58::decode(sig_str.substr(offset, sig_len), sig_decoded), error::wallet_internal_error, "Signature decoding error");
      THROW_WALLET_EXCEPTION_IF(sizeof(crypto::signature) != sig_decoded.size(), error::wallet_internal_error, "Signature decoding error");
      memcpy(&signatures.back()[j], sig_decoded.data(), sizeof(crypto::signature));
      offset += sig_len;
    }
  }

  // get signature prefix hash
  std::string sig_prefix_data((const char*)&txid, sizeof(crypto::hash));
  sig_prefix_data += message;
  crypto::hash sig_prefix_hash;
  crypto::cn_fast_hash(sig_prefix_data.data(), sig_prefix_data.size(), sig_prefix_hash);

  std::vector<std::vector<crypto::signature>>::const_iterator sig_iter = signatures.cbegin();
  for(size_t i = 0; i < tx.vin.size(); ++i)
  {
    const txin_to_key* const in_key = boost::get<txin_to_key>(std::addressof(tx.vin[i]));
    if (in_key == nullptr)
      continue;

    // get output pubkeys in the ring
    COMMAND_RPC_GET_OUTPUTS_BIN::request req = AUTO_VAL_INIT(req);
    const std::vector<uint64_t> absolute_offsets = cryptonote::relative_output_offsets_to_absolute(in_key->key_offsets);
    req.outputs.resize(absolute_offsets.size());
    for (size_t j = 0; j < absolute_offsets.size(); ++j)
    {
      req.outputs[j].amount = in_key->amount;
      req.outputs[j].index = absolute_offsets[j];
    }
    COMMAND_RPC_GET_OUTPUTS_BIN::response res = AUTO_VAL_INIT(res);
    bool r;
    {
      const boost::lock_guard<boost::mutex> lock{m_daemon_rpc_mutex};
      r = epee::net_utils::invoke_http_bin("/get_outs.bin", req, res, m_http_client, rpc_timeout);
    }
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_outs.bin");
    THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_outs.bin");
    THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::wallet_internal_error, "get_outs.bin");
    THROW_WALLET_EXCEPTION_IF(res.outs.size() != req.outputs.size(), error::wallet_internal_error,
      "daemon returned wrong response for get_outs.bin, wrong amounts count = " +
      std::to_string(res.outs.size()) + ", expected " +  std::to_string(req.outputs.size()));

    // copy pointers
    std::vector<const crypto::public_key *> p_output_keys;
    for (const COMMAND_RPC_GET_OUTPUTS_BIN::outkey &out : res.outs)
      p_output_keys.push_back(&out.key);

    // check this ring
    if (!crypto::check_ring_signature(sig_prefix_hash, in_key->k_image, p_output_keys, sig_iter->data()))
      return false;
    ++sig_iter;
  }
  THROW_WALLET_EXCEPTION_IF(sig_iter != signatures.cend(), error::wallet_internal_error, "Signature iterator didn't reach the end");
  return true;
}
//----------------------------------------------------------------------------------------------------

void wallet::check_tx_key(const crypto::hash &txid, const crypto::secret_key &tx_key, const std::vector<crypto::secret_key> &additional_tx_keys, const cryptonote::account_public_address &address, uint64_t &received, uint64_t& received_token, bool &in_pool, uint64_t &confirmations)
{
  crypto::key_derivation derivation;
  THROW_WALLET_EXCEPTION_IF(!crypto::generate_key_derivation(address.m_view_public_key, tx_key, derivation), error::wallet_internal_error,
    "Failed to generate key derivation from supplied parameters");

  std::vector<crypto::key_derivation> additional_derivations;
  additional_derivations.resize(additional_tx_keys.size());
  for (size_t i = 0; i < additional_tx_keys.size(); ++i)
    THROW_WALLET_EXCEPTION_IF(!crypto::generate_key_derivation(address.m_view_public_key, additional_tx_keys[i], additional_derivations[i]), error::wallet_internal_error,
      "Failed to generate key derivation from supplied parameters");

  check_tx_key_helper(txid, derivation, additional_derivations, address, received, received_token, in_pool, confirmations);
}

void wallet::check_tx_key_helper(const crypto::hash &txid, const crypto::key_derivation &derivation, const std::vector<crypto::key_derivation> &additional_derivations, const cryptonote::account_public_address &address, uint64_t &received, uint64_t& received_token, bool &in_pool, uint64_t &confirmations)
{
  COMMAND_RPC_GET_TRANSACTIONS::request req;
  COMMAND_RPC_GET_TRANSACTIONS::response res;
  req.txs_hashes.push_back(epee::string_tools::pod_to_hex(txid));
  req.decode_as_json = false;
  req.prune = false;
  m_daemon_rpc_mutex.lock();
  bool ok = epee::net_utils::invoke_http_json("/gettransactions", req, res, m_http_client);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!ok || (res.txs.size() != 1 && res.txs_as_hex.size() != 1),
    error::wallet_internal_error, "Failed to get transaction from daemon");

  cryptonote::blobdata tx_data;
  if (res.txs.size() == 1)
    ok = string_tools::parse_hexstr_to_binbuff(res.txs.front().as_hex, tx_data);
  else
    ok = string_tools::parse_hexstr_to_binbuff(res.txs_as_hex.front(), tx_data);
  THROW_WALLET_EXCEPTION_IF(!ok, error::wallet_internal_error, "Failed to parse transaction from daemon");

  crypto::hash tx_hash, tx_prefix_hash;
  cryptonote::transaction tx;
  THROW_WALLET_EXCEPTION_IF(!cryptonote::parse_and_validate_tx_from_blob(tx_data, tx, tx_hash, tx_prefix_hash), error::wallet_internal_error,
    "Failed to validate transaction from daemon");
  THROW_WALLET_EXCEPTION_IF(tx_hash != txid, error::wallet_internal_error,
    "Failed to get the right transaction from daemon");
  THROW_WALLET_EXCEPTION_IF(!additional_derivations.empty() && additional_derivations.size() != tx.vout.size(), error::wallet_internal_error,
    "The size of additional derivations is wrong");

  received = 0;
  received_token = 0;
  hw::device &hwdev =  m_account.get_device();
  for (size_t n = 0; n < tx.vout.size(); ++n)
  {
    crypto::public_key out_public_key;
    if(is_token_output(tx.vout[n].target)) {
      const cryptonote::txout_token_to_key* const out_key = boost::get<cryptonote::txout_token_to_key>(std::addressof(tx.vout[n].target));
      if (!out_key)
        continue;
      out_public_key = out_key->key;
    }
    else {
      const cryptonote::txout_to_key* const out_key = boost::get<cryptonote::txout_to_key>(std::addressof(tx.vout[n].target));
      if (!out_key)
        continue;
      out_public_key = out_key->key;
    }

    crypto::public_key derived_out_key;

    bool r = hwdev.derive_public_key(derivation, n, address.m_spend_public_key, derived_out_key);
    THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to derive public key");
    bool found = out_public_key == derived_out_key;
    crypto::key_derivation found_derivation = derivation;
    if (!found && !additional_derivations.empty())
    {
      r = hwdev.derive_public_key(additional_derivations[n], n, address.m_spend_public_key, derived_out_key);
      THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to derive public key");
      found = out_public_key == derived_out_key;
      found_derivation = additional_derivations[n];
    }

    if (found)
    {
      uint64_t amount;
      uint64_t amount_token = 0;
      if (tx.version == 1 || tx.rct_signatures.type == rct::RCTTypeNull)
      {
        amount = tx.vout[n].amount;
        amount_token = tx.vout[n].token_amount;
      }
      else
      {
        crypto::secret_key scalar1;
        hwdev.derivation_to_scalar(found_derivation, n, scalar1);
        rct::ecdhTuple ecdh_info = tx.rct_signatures.ecdhInfo[n];
        hwdev.ecdhDecode(ecdh_info, rct::sk2rct(scalar1));
        const rct::key C = tx.rct_signatures.outPk[n].mask;
        rct::key Ctmp;
        rct::addKeys2(Ctmp, ecdh_info.mask, ecdh_info.amount, rct::H);
        if (rct::equalKeys(C, Ctmp))
          amount = rct::h2d(ecdh_info.amount);
        else
          amount = 0;
      }
      received += amount;
      received_token += amount_token;
    }
  }

  in_pool = res.txs.front().in_pool;
  confirmations = (uint64_t)-1;
  if (!in_pool)
  {
    std::string err;
    uint64_t bc_height = get_daemon_blockchain_height(err);
    if (err.empty())
      confirmations = bc_height - (res.txs.front().block_height + 1);
  }
}

std::string wallet::get_tx_proof(const crypto::hash &txid, const cryptonote::account_public_address &address, bool is_subaddress, const std::string &message)
{
  // determine if the address is found in the subaddress hash table (i.e. whether the proof is outbound or inbound)
  const bool is_out = m_subaddresses.count(address.m_spend_public_key) == 0;

  std::string prefix_data((const char*)&txid, sizeof(crypto::hash));
  prefix_data += message;
  crypto::hash prefix_hash;
  crypto::cn_fast_hash(prefix_data.data(), prefix_data.size(), prefix_hash);

  std::vector<crypto::public_key> shared_secret;
  std::vector<crypto::signature> sig;
  std::string sig_str;
  if (is_out)
  {
    crypto::secret_key tx_key;
    std::vector<crypto::secret_key> additional_tx_keys;
    THROW_WALLET_EXCEPTION_IF(!get_tx_key(txid, tx_key, additional_tx_keys), error::wallet_internal_error, "Tx secret key wasn't found in the wallet file.");

    const size_t num_sigs = 1 + additional_tx_keys.size();
    shared_secret.resize(num_sigs);
    sig.resize(num_sigs);

    shared_secret[0] = rct::rct2pk(rct::scalarmultKey(rct::pk2rct(address.m_view_public_key), rct::sk2rct(tx_key)));
    crypto::public_key tx_pub_key;
    if (is_subaddress)
    {
      tx_pub_key = rct2pk(rct::scalarmultKey(rct::pk2rct(address.m_spend_public_key), rct::sk2rct(tx_key)));
      crypto::generate_tx_proof(prefix_hash, tx_pub_key, address.m_view_public_key, address.m_spend_public_key, shared_secret[0], tx_key, sig[0]);
    }
    else
    {
      crypto::secret_key_to_public_key(tx_key, tx_pub_key);
      crypto::generate_tx_proof(prefix_hash, tx_pub_key, address.m_view_public_key, boost::none, shared_secret[0], tx_key, sig[0]);
    }
    for (size_t i = 1; i < num_sigs; ++i)
    {
      shared_secret[i] = rct::rct2pk(rct::scalarmultKey(rct::pk2rct(address.m_view_public_key), rct::sk2rct(additional_tx_keys[i - 1])));
      if (is_subaddress)
      {
        tx_pub_key = rct2pk(rct::scalarmultKey(rct::pk2rct(address.m_spend_public_key), rct::sk2rct(additional_tx_keys[i - 1])));
        crypto::generate_tx_proof(prefix_hash, tx_pub_key, address.m_view_public_key, address.m_spend_public_key, shared_secret[i], additional_tx_keys[i - 1], sig[i]);
      }
      else
      {
        crypto::secret_key_to_public_key(additional_tx_keys[i - 1], tx_pub_key);
        crypto::generate_tx_proof(prefix_hash, tx_pub_key, address.m_view_public_key, boost::none, shared_secret[i], additional_tx_keys[i - 1], sig[i]);
      }
    }
    sig_str = std::string("OutProofV1");
  }
  else
  {
    // fetch tx pubkey from the daemon
    COMMAND_RPC_GET_TRANSACTIONS::request req;
    COMMAND_RPC_GET_TRANSACTIONS::response res;
    req.txs_hashes.push_back(epee::string_tools::pod_to_hex(txid));
    req.decode_as_json = false;
    req.prune = false;
    m_daemon_rpc_mutex.lock();
    bool ok = net_utils::invoke_http_json("/gettransactions", req, res, m_http_client);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!ok || (res.txs.size() != 1 && res.txs_as_hex.size() != 1),
      error::wallet_internal_error, "Failed to get transaction from daemon");

    cryptonote::blobdata tx_data;
    if (res.txs.size() == 1)
      ok = string_tools::parse_hexstr_to_binbuff(res.txs.front().as_hex, tx_data);
    else
      ok = string_tools::parse_hexstr_to_binbuff(res.txs_as_hex.front(), tx_data);
    THROW_WALLET_EXCEPTION_IF(!ok, error::wallet_internal_error, "Failed to parse transaction from daemon");

    crypto::hash tx_hash, tx_prefix_hash;
    cryptonote::transaction tx;
    THROW_WALLET_EXCEPTION_IF(!cryptonote::parse_and_validate_tx_from_blob(tx_data, tx, tx_hash, tx_prefix_hash), error::wallet_internal_error,
      "Failed to validate transaction from daemon");
    THROW_WALLET_EXCEPTION_IF(tx_hash != txid, error::wallet_internal_error, "Failed to get the right transaction from daemon");

    crypto::public_key tx_pub_key = get_tx_pub_key_from_extra(tx);
    THROW_WALLET_EXCEPTION_IF(tx_pub_key == null_pkey, error::wallet_internal_error, "Tx pubkey was not found");

    std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(tx);
    const size_t num_sigs = 1 + additional_tx_pub_keys.size();
    shared_secret.resize(num_sigs);
    sig.resize(num_sigs);

    const crypto::secret_key& a = m_account.get_keys().m_view_secret_key;
    shared_secret[0] = rct::rct2pk(rct::scalarmultKey(rct::pk2rct(tx_pub_key), rct::sk2rct(a)));
    if (is_subaddress)
    {
      crypto::generate_tx_proof(prefix_hash, address.m_view_public_key, tx_pub_key, address.m_spend_public_key, shared_secret[0], a, sig[0]);
    }
    else
    {
      crypto::generate_tx_proof(prefix_hash, address.m_view_public_key, tx_pub_key, boost::none, shared_secret[0], a, sig[0]);
    }
    for (size_t i = 1; i < num_sigs; ++i)
    {
      shared_secret[i] = rct::rct2pk(rct::scalarmultKey(rct::pk2rct(additional_tx_pub_keys[i - 1]), rct::sk2rct(a)));
      if (is_subaddress)
      {
        crypto::generate_tx_proof(prefix_hash, address.m_view_public_key, additional_tx_pub_keys[i - 1], address.m_spend_public_key, shared_secret[i], a, sig[i]);
      }
      else
      {
        crypto::generate_tx_proof(prefix_hash, address.m_view_public_key, additional_tx_pub_keys[i - 1], boost::none, shared_secret[i], a, sig[i]);
      }
    }
    sig_str = std::string("InProofV1");
  }
  const size_t num_sigs = shared_secret.size();

  // check if this address actually received any funds
  crypto::key_derivation derivation;
  THROW_WALLET_EXCEPTION_IF(!crypto::generate_key_derivation(shared_secret[0], rct::rct2sk(rct::I), derivation), error::wallet_internal_error, "Failed to generate key derivation");
  std::vector<crypto::key_derivation> additional_derivations(num_sigs - 1);
  for (size_t i = 1; i < num_sigs; ++i)
    THROW_WALLET_EXCEPTION_IF(!crypto::generate_key_derivation(shared_secret[i], rct::rct2sk(rct::I), additional_derivations[i - 1]), error::wallet_internal_error, "Failed to generate key derivation");
  uint64_t received;
  uint64_t received_token;
  bool in_pool;
  uint64_t confirmations;
  check_tx_key_helper(txid, derivation, additional_derivations, address, received, received_token, in_pool, confirmations);
  THROW_WALLET_EXCEPTION_IF(!received && !received_token, error::wallet_internal_error, tr("No funds received in this tx."));

  // concatenate all signature strings
  for (size_t i = 0; i < num_sigs; ++i)
    sig_str +=
      tools::base58::encode(std::string((const char *)&shared_secret[i], sizeof(crypto::public_key))) +
      tools::base58::encode(std::string((const char *)&sig[i], sizeof(crypto::signature)));
  return sig_str;
}

bool wallet::check_tx_proof(const crypto::hash &txid, const cryptonote::account_public_address &address, bool is_subaddress, const std::string &message, const std::string &sig_str, uint64_t &received, uint64_t &received_token, bool &in_pool, uint64_t &confirmations)
{
  const bool is_out = sig_str.substr(0, 3) == "Out";
  const std::string header = is_out ? "OutProofV1" : "InProofV1";
  const size_t header_len = header.size();
  THROW_WALLET_EXCEPTION_IF(sig_str.size() < header_len || sig_str.substr(0, header_len) != header, error::wallet_internal_error,
    "Signature header check error");

  // decode base58
  std::vector<crypto::public_key> shared_secret(1);
  std::vector<crypto::signature> sig(1);
  const size_t pk_len = tools::base58::encode(std::string((const char *)&shared_secret[0], sizeof(crypto::public_key))).size();
  const size_t sig_len = tools::base58::encode(std::string((const char *)&sig[0], sizeof(crypto::signature))).size();
  const size_t num_sigs = (sig_str.size() - header_len) / (pk_len + sig_len);
  THROW_WALLET_EXCEPTION_IF(sig_str.size() != header_len + num_sigs * (pk_len + sig_len), error::wallet_internal_error,
    "Wrong signature size");
  shared_secret.resize(num_sigs);
  sig.resize(num_sigs);
  for (size_t i = 0; i < num_sigs; ++i)
  {
    std::string pk_decoded;
    std::string sig_decoded;
    const size_t offset = header_len + i * (pk_len + sig_len);
    THROW_WALLET_EXCEPTION_IF(!tools::base58::decode(sig_str.substr(offset, pk_len), pk_decoded), error::wallet_internal_error,
      "Signature decoding error");
    THROW_WALLET_EXCEPTION_IF(!tools::base58::decode(sig_str.substr(offset + pk_len, sig_len), sig_decoded), error::wallet_internal_error,
      "Signature decoding error");
    THROW_WALLET_EXCEPTION_IF(sizeof(crypto::public_key) != pk_decoded.size() || sizeof(crypto::signature) != sig_decoded.size(), error::wallet_internal_error,
      "Signature decoding error");
    memcpy(&shared_secret[i], pk_decoded.data(), sizeof(crypto::public_key));
    memcpy(&sig[i], sig_decoded.data(), sizeof(crypto::signature));
  }

  // fetch tx pubkey from the daemon
  COMMAND_RPC_GET_TRANSACTIONS::request req;
  COMMAND_RPC_GET_TRANSACTIONS::response res;
  req.txs_hashes.push_back(epee::string_tools::pod_to_hex(txid));
  req.decode_as_json = false;
  req.prune = false;
  m_daemon_rpc_mutex.lock();
  bool ok = net_utils::invoke_http_json("/gettransactions", req, res, m_http_client);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!ok || (res.txs.size() != 1 && res.txs_as_hex.size() != 1),
    error::wallet_internal_error, "Failed to get transaction from daemon");

  cryptonote::blobdata tx_data;
  if (res.txs.size() == 1)
    ok = string_tools::parse_hexstr_to_binbuff(res.txs.front().as_hex, tx_data);
  else
    ok = string_tools::parse_hexstr_to_binbuff(res.txs_as_hex.front(), tx_data);
  THROW_WALLET_EXCEPTION_IF(!ok, error::wallet_internal_error, "Failed to parse transaction from daemon");

  crypto::hash tx_hash, tx_prefix_hash;
  cryptonote::transaction tx;
  THROW_WALLET_EXCEPTION_IF(!cryptonote::parse_and_validate_tx_from_blob(tx_data, tx, tx_hash, tx_prefix_hash), error::wallet_internal_error,
    "Failed to validate transaction from daemon");
  THROW_WALLET_EXCEPTION_IF(tx_hash != txid, error::wallet_internal_error, "Failed to get the right transaction from daemon");

  crypto::public_key tx_pub_key = get_tx_pub_key_from_extra(tx);
  THROW_WALLET_EXCEPTION_IF(tx_pub_key == null_pkey, error::wallet_internal_error, "Tx pubkey was not found");

  std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(tx);
  THROW_WALLET_EXCEPTION_IF(additional_tx_pub_keys.size() + 1 != num_sigs, error::wallet_internal_error, "Signature size mismatch with additional tx pubkeys");

  std::string prefix_data((const char*)&txid, sizeof(crypto::hash));
  prefix_data += message;
  crypto::hash prefix_hash;
  crypto::cn_fast_hash(prefix_data.data(), prefix_data.size(), prefix_hash);

  // check signature
  std::vector<int> good_signature(num_sigs, 0);
  if (is_out)
  {
    good_signature[0] = is_subaddress ?
      crypto::check_tx_proof(prefix_hash, tx_pub_key, address.m_view_public_key, address.m_spend_public_key, shared_secret[0], sig[0]) :
      crypto::check_tx_proof(prefix_hash, tx_pub_key, address.m_view_public_key, boost::none, shared_secret[0], sig[0]);

    for (size_t i = 0; i < additional_tx_pub_keys.size(); ++i)
    {
      good_signature[i + 1] = is_subaddress ?
        crypto::check_tx_proof(prefix_hash, additional_tx_pub_keys[i], address.m_view_public_key, address.m_spend_public_key, shared_secret[i + 1], sig[i + 1]) :
        crypto::check_tx_proof(prefix_hash, additional_tx_pub_keys[i], address.m_view_public_key, boost::none, shared_secret[i + 1], sig[i + 1]);
    }
  }
  else
  {
    good_signature[0] = is_subaddress ?
      crypto::check_tx_proof(prefix_hash, address.m_view_public_key, tx_pub_key, address.m_spend_public_key, shared_secret[0], sig[0]) :
      crypto::check_tx_proof(prefix_hash, address.m_view_public_key, tx_pub_key, boost::none, shared_secret[0], sig[0]);

    for (size_t i = 0; i < additional_tx_pub_keys.size(); ++i)
    {
      good_signature[i + 1] = is_subaddress ?
        crypto::check_tx_proof(prefix_hash, address.m_view_public_key, additional_tx_pub_keys[i], address.m_spend_public_key, shared_secret[i + 1], sig[i + 1]) :
        crypto::check_tx_proof(prefix_hash, address.m_view_public_key, additional_tx_pub_keys[i], boost::none, shared_secret[i + 1], sig[i + 1]);
    }
  }

  if (std::any_of(good_signature.begin(), good_signature.end(), [](int i) { return i > 0; }))
  {
    // obtain key derivation by multiplying scalar 1 to the shared secret
    crypto::key_derivation derivation;
    if (good_signature[0])
      THROW_WALLET_EXCEPTION_IF(!crypto::generate_key_derivation(shared_secret[0], rct::rct2sk(rct::I), derivation), error::wallet_internal_error, "Failed to generate key derivation");

    std::vector<crypto::key_derivation> additional_derivations(num_sigs - 1);
    for (size_t i = 1; i < num_sigs; ++i)
      if (good_signature[i])
        THROW_WALLET_EXCEPTION_IF(!crypto::generate_key_derivation(shared_secret[i], rct::rct2sk(rct::I), additional_derivations[i - 1]), error::wallet_internal_error, "Failed to generate key derivation");

    check_tx_key_helper(txid, derivation, additional_derivations, address, received, received_token, in_pool, confirmations);
    return true;
  }
  return false;
}

std::string wallet::get_reserve_proof(const boost::optional<std::pair<uint32_t, uint64_t>> &account_minreserve, const std::string &message, bool token)
{
  THROW_WALLET_EXCEPTION_IF(m_watch_only, error::wallet_internal_error, "Reserve proof can only be generated by a full wallet");
  if(!token) {
    THROW_WALLET_EXCEPTION_IF(balance_all() == 0, error::wallet_internal_error, "Zero balance");
    THROW_WALLET_EXCEPTION_IF(account_minreserve && balance(account_minreserve->first) < account_minreserve->second, error::wallet_internal_error,
      "Not enough balance in this account for the requested minimum reserve amount");
  }
  else {
    THROW_WALLET_EXCEPTION_IF(token_balance_all() == 0, error::wallet_internal_error, "Zero balance");
    THROW_WALLET_EXCEPTION_IF(account_minreserve && token_balance(account_minreserve->first) < account_minreserve->second, error::wallet_internal_error,
                              "Not enough balance in this account for the requested minimum reserve amount");
  }
  // determine which outputs to include in the proof
  std::vector<size_t> selected_transfers;
  for (size_t i = 0; i < m_transfers.size(); ++i)
  {
    const transfer_details &td = m_transfers[i];
    if (!td.m_spent && (!account_minreserve || account_minreserve->first == td.m_subaddr_index.major))
      selected_transfers.push_back(i);
  }

  if (account_minreserve)
  {
    if(!token) {
      // minimize the number of outputs included in the proof, by only picking the N largest outputs that can cover the requested min reserve amount
      std::sort(selected_transfers.begin(), selected_transfers.end(), [&](const size_t a, const size_t b)
        { return m_transfers[a].amount() > m_transfers[b].amount(); });
      while (selected_transfers.size() >= 2 && m_transfers[selected_transfers[1]].amount() >= account_minreserve->second)
        selected_transfers.erase(selected_transfers.begin());
      size_t sz = 0;
      uint64_t total = 0;
      while (total < account_minreserve->second)
      {
        total += m_transfers[selected_transfers[sz]].amount();
        ++sz;
      }
      selected_transfers.resize(sz);
    }
    else {
      // minimize the number of outputs included in the proof, by only picking the N largest outputs that can cover the requested min reserve amount
      std::sort(selected_transfers.begin(), selected_transfers.end(), [&](const size_t a, const size_t b)
      { return m_transfers[a].token_amount() > m_transfers[b].token_amount(); });
      while (selected_transfers.size() >= 2 && m_transfers[selected_transfers[1]].token_amount() >= account_minreserve->second)
        selected_transfers.erase(selected_transfers.begin());
      size_t sz = 0;
      uint64_t total = 0;
      while (total < account_minreserve->second)
      {
        total += m_transfers[selected_transfers[sz]].token_amount();
        ++sz;
      }
      selected_transfers.resize(sz);
    }
  }

  // compute signature prefix hash
  std::string prefix_data = message;
  prefix_data.append((const char*)&m_account.get_keys().m_account_address, sizeof(cryptonote::account_public_address));
  for (size_t i = 0; i < selected_transfers.size(); ++i)
  {
    prefix_data.append((const char*)&m_transfers[selected_transfers[i]].m_key_image, sizeof(crypto::key_image));
  }
  crypto::hash prefix_hash;
  crypto::cn_fast_hash(prefix_data.data(), prefix_data.size(), prefix_hash);

  // generate proof entries
  std::vector<reserve_proof_entry> proofs(selected_transfers.size());
  std::unordered_set<cryptonote::subaddress_index> subaddr_indices = { {0,0} };
  for (size_t i = 0; i < selected_transfers.size(); ++i)
  {
    const transfer_details &td = m_transfers[selected_transfers[i]];
    reserve_proof_entry& proof = proofs[i];
    proof.txid = td.m_txid;
    proof.index_in_tx = td.m_internal_output_index;
    proof.key_image = td.m_key_image;
    subaddr_indices.insert(td.m_subaddr_index);

    // get tx pub key
    const crypto::public_key tx_pub_key = get_tx_pub_key_from_extra(td.m_tx, td.m_pk_index);
    THROW_WALLET_EXCEPTION_IF(tx_pub_key == crypto::null_pkey, error::wallet_internal_error, "The tx public key isn't found");
    const std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(td.m_tx);

    // determine which tx pub key was used for deriving the output key
    const crypto::public_key *tx_pub_key_used = &tx_pub_key;
    for (int i = 0; i < 2; ++i)
    {
      proof.shared_secret = rct::rct2pk(rct::scalarmultKey(rct::pk2rct(*tx_pub_key_used), rct::sk2rct(m_account.get_keys().m_view_secret_key)));
      crypto::key_derivation derivation;
      THROW_WALLET_EXCEPTION_IF(!crypto::generate_key_derivation(proof.shared_secret, rct::rct2sk(rct::I), derivation),
        error::wallet_internal_error, "Failed to generate key derivation");
      crypto::public_key subaddress_spendkey;
      THROW_WALLET_EXCEPTION_IF(!derive_subaddress_public_key(td.get_public_key(), derivation, proof.index_in_tx, subaddress_spendkey),
        error::wallet_internal_error, "Failed to derive subaddress public key");
      if (m_subaddresses.count(subaddress_spendkey) == 1)
        break;
      THROW_WALLET_EXCEPTION_IF(additional_tx_pub_keys.empty(), error::wallet_internal_error,
        "Normal tx pub key doesn't derive the expected output, while the additional tx pub keys are empty");
      THROW_WALLET_EXCEPTION_IF(i == 1, error::wallet_internal_error,
        "Neither normal tx pub key nor additional tx pub key derive the expected output key");
      tx_pub_key_used = &additional_tx_pub_keys[proof.index_in_tx];
    }

    // generate signature for shared secret
    crypto::generate_tx_proof(prefix_hash, m_account.get_keys().m_account_address.m_view_public_key, *tx_pub_key_used, boost::none, proof.shared_secret, m_account.get_keys().m_view_secret_key, proof.shared_secret_sig);

    // derive ephemeral secret key
    crypto::key_image ki;
    cryptonote::keypair ephemeral;
    const bool r = cryptonote::generate_key_image_helper(m_account.get_keys(), m_subaddresses, td.get_public_key(), tx_pub_key,  additional_tx_pub_keys, td.m_internal_output_index, ephemeral, ki, m_account.get_device());
    THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key image");
    THROW_WALLET_EXCEPTION_IF(ephemeral.pub != td.get_public_key(), error::wallet_internal_error, "Derived public key doesn't agree with the stored one");

    // generate signature for key image
    const std::vector<const crypto::public_key*> pubs = { &ephemeral.pub };
    crypto::generate_ring_signature(prefix_hash, td.m_key_image, &pubs[0], 1, ephemeral.sec, 0, &proof.key_image_sig);
  }

  // collect all subaddress spend keys that received those outputs and generate their signatures
  std::unordered_map<crypto::public_key, crypto::signature> subaddr_spendkeys;
  for (const cryptonote::subaddress_index &index : subaddr_indices)
  {
    crypto::secret_key subaddr_spend_skey = m_account.get_keys().m_spend_secret_key;
    if (!index.is_zero())
    {
      crypto::secret_key m = m_account.get_device().get_subaddress_secret_key(m_account.get_keys().m_view_secret_key, index);
      crypto::secret_key tmp = subaddr_spend_skey;
      sc_add((unsigned char*)&subaddr_spend_skey, (unsigned char*)&m, (unsigned char*)&tmp);
    }
    crypto::public_key subaddr_spend_pkey;
    secret_key_to_public_key(subaddr_spend_skey, subaddr_spend_pkey);
    crypto::generate_signature(prefix_hash, subaddr_spend_pkey, subaddr_spend_skey, subaddr_spendkeys[subaddr_spend_pkey]);
  }

  // serialize & encode
  std::ostringstream oss;
  boost::archive::portable_binary_oarchive ar(oss);
  ar << proofs << subaddr_spendkeys;
  return "ReserveProofV1" + tools::base58::encode(oss.str());
}

bool wallet::check_reserve_proof(const cryptonote::account_public_address &address, const std::string &message, const std::string &sig_str, uint64_t &total, uint64_t &spent, uint64_t& token_total, uint64_t& token_spent)
{
  uint32_t rpc_version;
  THROW_WALLET_EXCEPTION_IF(!check_connection(&rpc_version), error::wallet_internal_error, "Failed to connect to daemon: " + get_daemon_address());
  THROW_WALLET_EXCEPTION_IF(rpc_version < MAKE_CORE_RPC_VERSION(1, 0), error::wallet_internal_error, "Daemon RPC version is too old");

  static constexpr char header[] = "ReserveProofV1";
  THROW_WALLET_EXCEPTION_IF(!boost::string_ref{sig_str}.starts_with(header), error::wallet_internal_error,
    "Signature header check error");

  std::string sig_decoded;
  THROW_WALLET_EXCEPTION_IF(!tools::base58::decode(sig_str.substr(std::strlen(header)), sig_decoded), error::wallet_internal_error,
    "Signature decoding error");

  std::istringstream iss(sig_decoded);
  boost::archive::portable_binary_iarchive ar(iss);
  std::vector<reserve_proof_entry> proofs;
  std::unordered_map<crypto::public_key, crypto::signature> subaddr_spendkeys;
  ar >> proofs >> subaddr_spendkeys;

  THROW_WALLET_EXCEPTION_IF(subaddr_spendkeys.count(address.m_spend_public_key) == 0, error::wallet_internal_error,
    "The given address isn't found in the proof");

  // compute signature prefix hash
  std::string prefix_data = message;
  prefix_data.append((const char*)&address, sizeof(cryptonote::account_public_address));
  for (size_t i = 0; i < proofs.size(); ++i)
  {
    prefix_data.append((const char*)&proofs[i].key_image, sizeof(crypto::key_image));
  }
  crypto::hash prefix_hash;
  crypto::cn_fast_hash(prefix_data.data(), prefix_data.size(), prefix_hash);

  // fetch txes from daemon
  COMMAND_RPC_GET_TRANSACTIONS::request gettx_req;
  COMMAND_RPC_GET_TRANSACTIONS::response gettx_res;
  for (size_t i = 0; i < proofs.size(); ++i)
    gettx_req.txs_hashes.push_back(epee::string_tools::pod_to_hex(proofs[i].txid));
  gettx_req.decode_as_json = false;
  gettx_req.prune = false;
  m_daemon_rpc_mutex.lock();
  bool ok = net_utils::invoke_http_json("/gettransactions", gettx_req, gettx_res, m_http_client);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!ok || gettx_res.txs.size() != proofs.size(),
    error::wallet_internal_error, "Failed to get transaction from daemon");

  // check spent status
  COMMAND_RPC_IS_KEY_IMAGE_SPENT::request kispent_req;
  COMMAND_RPC_IS_KEY_IMAGE_SPENT::response kispent_res;
  for (size_t i = 0; i < proofs.size(); ++i)
    kispent_req.key_images.push_back(epee::string_tools::pod_to_hex(proofs[i].key_image));
  m_daemon_rpc_mutex.lock();
  ok = epee::net_utils::invoke_http_json("/is_key_image_spent", kispent_req, kispent_res, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!ok || kispent_res.spent_status.size() != proofs.size(),
    error::wallet_internal_error, "Failed to get key image spent status from daemon");

  total = spent = 0;
  token_total = token_spent = 0;
  for (size_t i = 0; i < proofs.size(); ++i)
  {
    const reserve_proof_entry& proof = proofs[i];
    THROW_WALLET_EXCEPTION_IF(gettx_res.txs[i].in_pool, error::wallet_internal_error, "Tx is unconfirmed");

    cryptonote::blobdata tx_data;
    ok = string_tools::parse_hexstr_to_binbuff(gettx_res.txs[i].as_hex, tx_data);
    THROW_WALLET_EXCEPTION_IF(!ok, error::wallet_internal_error, "Failed to parse transaction from daemon");

    crypto::hash tx_hash, tx_prefix_hash;
    cryptonote::transaction tx;
    THROW_WALLET_EXCEPTION_IF(!cryptonote::parse_and_validate_tx_from_blob(tx_data, tx, tx_hash, tx_prefix_hash), error::wallet_internal_error,
      "Failed to validate transaction from daemon");
    THROW_WALLET_EXCEPTION_IF(tx_hash != proof.txid, error::wallet_internal_error, "Failed to get the right transaction from daemon");

    THROW_WALLET_EXCEPTION_IF(proof.index_in_tx >= tx.vout.size(), error::wallet_internal_error, "index_in_tx is out of bound");

    crypto::public_key out_public_key;
    if(is_token_output(tx.vout[proof.index_in_tx].target)) {
      const cryptonote::txout_token_to_key* const out_key = boost::get<cryptonote::txout_token_to_key>(std::addressof(tx.vout[proof.index_in_tx].target));
      THROW_WALLET_EXCEPTION_IF(!out_key, error::wallet_internal_error, "Output key wasn't found");
      out_public_key = out_key->key;
    }
    else {
      const cryptonote::txout_to_key* const out_key = boost::get<cryptonote::txout_to_key>(std::addressof(tx.vout[proof.index_in_tx].target));
      THROW_WALLET_EXCEPTION_IF(!out_key, error::wallet_internal_error, "Output key wasn't found");
      out_public_key = out_key->key;
    }

    // get tx pub key
    const crypto::public_key tx_pub_key = get_tx_pub_key_from_extra(tx);
    THROW_WALLET_EXCEPTION_IF(tx_pub_key == crypto::null_pkey, error::wallet_internal_error, "The tx public key isn't found");
    const std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(tx);

    // check singature for shared secret
    ok = crypto::check_tx_proof(prefix_hash, address.m_view_public_key, tx_pub_key, boost::none, proof.shared_secret, proof.shared_secret_sig);
    if (!ok && additional_tx_pub_keys.size() == tx.vout.size())
      ok = crypto::check_tx_proof(prefix_hash, address.m_view_public_key, additional_tx_pub_keys[proof.index_in_tx], boost::none, proof.shared_secret, proof.shared_secret_sig);
    if (!ok)
      return false;

    // check signature for key image
    const std::vector<const crypto::public_key*> pubs = { &out_public_key };
    ok = crypto::check_ring_signature(prefix_hash, proof.key_image, &pubs[0], 1, &proof.key_image_sig);
    if (!ok)
      return false;

    // check if the address really received the fund
    crypto::key_derivation derivation;
    THROW_WALLET_EXCEPTION_IF(!crypto::generate_key_derivation(proof.shared_secret, rct::rct2sk(rct::I), derivation), error::wallet_internal_error, "Failed to generate key derivation");
    crypto::public_key subaddr_spendkey;
    crypto::derive_subaddress_public_key(out_public_key, derivation, proof.index_in_tx, subaddr_spendkey);
    THROW_WALLET_EXCEPTION_IF(subaddr_spendkeys.count(subaddr_spendkey) == 0, error::wallet_internal_error,
      "The address doesn't seem to have received the fund");

    if(is_token_output(tx.vout[proof.index_in_tx].target)) {

      uint64_t token_amount = tx.vout[proof.index_in_tx].token_amount;
      token_total += token_amount;
      if (kispent_res.spent_status[i])
        token_spent += token_amount;
    }
    else {
      // check amount
      uint64_t amount = tx.vout[proof.index_in_tx].amount;
      total += amount;
      if (kispent_res.spent_status[i])
        spent += amount;
    }


  }

  // check signatures for all subaddress spend keys
  for (const auto &i : subaddr_spendkeys)
  {
    if (!crypto::check_signature(prefix_hash, i.first, i.second))
      return false;
  }
  return true;
}

std::string wallet::get_wallet_file() const
{
  return m_wallet_file;
}

std::string wallet::get_keys_file() const
{
  return m_keys_file;
}

std::string wallet::get_daemon_address() const
{
  return m_daemon_address;
}

uint64_t wallet::get_daemon_blockchain_height(string &err) const
{
  uint64_t height;

  boost::optional<std::string> result = m_node_rpc_proxy.get_height(height);
  if (result)
  {
    err = *result;
    return 0;
  }

  err = "";
  return height;
}

uint64_t wallet::get_daemon_blockchain_target_height(string &err)
{
  cryptonote::COMMAND_RPC_GET_INFO::request req_t = AUTO_VAL_INIT(req_t);
  cryptonote::COMMAND_RPC_GET_INFO::response resp_t = AUTO_VAL_INIT(resp_t);
  m_daemon_rpc_mutex.lock();
  bool ok = net_utils::invoke_http_json_rpc("/json_rpc", "get_info", req_t, resp_t, m_http_client);
  m_daemon_rpc_mutex.unlock();
  if (ok)
  {
    if (resp_t.status == CORE_RPC_STATUS_BUSY)
    {
      err = "daemon is busy. Please try again later.";
    }
    else if (resp_t.status != CORE_RPC_STATUS_OK)
    {
      err = resp_t.status;
    }
    else // success, cleaning up error message
    {
      err = "";
    }
  }
  else
  {
    err = "possibly lost connection to daemon";
  }
  return resp_t.target_height;
}

uint64_t wallet::get_approximate_blockchain_height() const
{
  // time of v2 fork
  const time_t fork_time = m_nettype == TESTNET ? 1448285909 : m_nettype == STAGENET ? (time_t)-1/*TODO*/ : 1458748658;
  // v2 fork block
  const uint64_t fork_block = m_nettype == TESTNET ? 624634 : m_nettype == STAGENET ? (uint64_t)-1/*TODO*/ : 1009827;
  // avg seconds per block
  const int seconds_per_block = DIFFICULTY_TARGET;
  // Calculated blockchain height
  uint64_t approx_blockchain_height = fork_block + (time(NULL) - fork_time)/seconds_per_block;
  // testnet got some huge rollbacks, so the estimation is way off
  static const uint64_t approximate_testnet_rolled_back_blocks = 148540;
  if (m_nettype == TESTNET && approx_blockchain_height > approximate_testnet_rolled_back_blocks)
    approx_blockchain_height -= approximate_testnet_rolled_back_blocks;
  LOG_PRINT_L2("Calculated blockchain height: " << approx_blockchain_height);
  return approx_blockchain_height;
}

void wallet::set_tx_note(const crypto::hash &txid, const std::string &note)
{
  m_tx_notes[txid] = note;
}

std::string wallet::get_tx_note(const crypto::hash &txid) const
{
  std::unordered_map<crypto::hash, std::string>::const_iterator i = m_tx_notes.find(txid);
  if (i == m_tx_notes.end())
    return std::string();
  return i->second;
}

void wallet::set_attribute(const std::string &key, const std::string &value)
{
  m_attributes[key] = value;
}

std::string wallet::get_attribute(const std::string &key) const
{
  std::unordered_map<std::string, std::string>::const_iterator i = m_attributes.find(key);
  if (i == m_attributes.end())
    return std::string();
  return i->second;
}

void wallet::set_description(const std::string &description)
{
  set_attribute(ATTRIBUTE_DESCRIPTION, description);
}

std::string wallet::get_description() const
{
  return get_attribute(ATTRIBUTE_DESCRIPTION);
}

const std::pair<std::map<std::string, std::string>, std::vector<std::string>>& wallet::get_account_tags()
{
  // ensure consistency
  if (m_account_tags.second.size() != get_num_subaddress_accounts())
    m_account_tags.second.resize(get_num_subaddress_accounts(), "");
  for (const std::string& tag : m_account_tags.second)
  {
    if (!tag.empty() && m_account_tags.first.count(tag) == 0)
      m_account_tags.first.insert({tag, ""});
  }
  for (auto i = m_account_tags.first.begin(); i != m_account_tags.first.end(); )
  {
    if (std::find(m_account_tags.second.begin(), m_account_tags.second.end(), i->first) == m_account_tags.second.end())
      i = m_account_tags.first.erase(i);
    else
      ++i;
  }
  return m_account_tags;
}

void wallet::set_account_tag(const std::set<uint32_t> account_indices, const std::string& tag)
{
  for (uint32_t account_index : account_indices)
  {
    THROW_WALLET_EXCEPTION_IF(account_index >= get_num_subaddress_accounts(), error::wallet_internal_error, "Account index out of bound");
    if (m_account_tags.second[account_index] == tag)
      MDEBUG("This tag is already assigned to this account");
    else
      m_account_tags.second[account_index] = tag;
  }
  get_account_tags();
}

void wallet::set_account_tag_description(const std::string& tag, const std::string& description)
{
  THROW_WALLET_EXCEPTION_IF(tag.empty(), error::wallet_internal_error, "Tag must not be empty");
  THROW_WALLET_EXCEPTION_IF(m_account_tags.first.count(tag) == 0, error::wallet_internal_error, "Tag is unregistered");
  m_account_tags.first[tag] = description;
}

std::string wallet::sign(const std::string &data) const
{
  crypto::hash hash;
  crypto::cn_fast_hash(data.data(), data.size(), hash);
  const cryptonote::account_keys &keys = m_account.get_keys();
  crypto::signature signature;
  crypto::generate_signature(hash, keys.m_account_address.m_spend_public_key, keys.m_spend_secret_key, signature);
  return std::string("SigV1") + tools::base58::encode(std::string((const char *)&signature, sizeof(signature)));
}

bool wallet::verify(const std::string &data, const cryptonote::account_public_address &address, const std::string &signature) const
{
  const size_t header_len = strlen("SigV1");
  if (signature.size() < header_len || signature.substr(0, header_len) != "SigV1") {
    LOG_PRINT_L0("Signature header check error");
    return false;
  }
  crypto::hash hash;
  crypto::cn_fast_hash(data.data(), data.size(), hash);
  std::string decoded;
  if (!tools::base58::decode(signature.substr(header_len), decoded)) {
    LOG_PRINT_L0("Signature decoding error");
    return false;
  }
  crypto::signature s;
  if (sizeof(s) != decoded.size()) {
    LOG_PRINT_L0("Signature decoding error");
    return false;
  }
  memcpy(&s, decoded.data(), sizeof(s));
  return crypto::check_signature(hash, address.m_spend_public_key, s);
}
//----------------------------------------------------------------------------------------------------
crypto::public_key wallet::get_tx_pub_key_from_received_outs(const tools::wallet::transfer_details &td) const
{
  std::vector<tx_extra_field> tx_extra_fields;
  if(!parse_tx_extra(td.m_tx.extra, tx_extra_fields))
  {
    // Extra may only be partially parsed, it's OK if tx_extra_fields contains public key
  }

  // Due to a previous bug, there might be more than one tx pubkey in extra, one being
  // the result of a previously discarded signature.
  // For speed, since scanning for outputs is a slow process, we check whether extra
  // contains more than one pubkey. If not, the first one is returned. If yes, they're
  // checked for whether they yield at least one output
  tx_extra_pub_key pub_key_field;
  THROW_WALLET_EXCEPTION_IF(!find_tx_extra_field_by_type(tx_extra_fields, pub_key_field, 0), error::wallet_internal_error,
      "Public key wasn't found in the transaction extra");
  const crypto::public_key tx_pub_key = pub_key_field.pub_key;
  bool two_found = find_tx_extra_field_by_type(tx_extra_fields, pub_key_field, 1);
  if (!two_found) {
    // easy case, just one found
    return tx_pub_key;
  }

  // more than one, loop and search
  const cryptonote::account_keys& keys = m_account.get_keys();
  size_t pk_index = 0;
  hw::device &hwdev = m_account.get_device();

  const std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(td.m_tx);
  std::vector<crypto::key_derivation> additional_derivations;
  for (size_t i = 0; i < additional_tx_pub_keys.size(); ++i)
  {
    additional_derivations.push_back({});
    bool r = hwdev.generate_key_derivation(additional_tx_pub_keys[i], keys.m_view_secret_key, additional_derivations.back());
    THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key derivation");
  }

  while (find_tx_extra_field_by_type(tx_extra_fields, pub_key_field, pk_index++)) {
    const crypto::public_key tx_pub_key = pub_key_field.pub_key;
    crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
    bool r = hwdev.generate_key_derivation(tx_pub_key, keys.m_view_secret_key, derivation);
    THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key derivation");

    for (size_t i = 0; i < td.m_tx.vout.size(); ++i)
    {
      tx_scan_info_t tx_scan_info = AUTO_VAL_INIT(tx_scan_info);
      check_acc_out_precomp(td.m_tx.vout[i], derivation, additional_derivations, i, tx_scan_info);
      if (!tx_scan_info.error && tx_scan_info.received)
        return tx_pub_key;
    }
  }

  // we found no key yielding an output
  THROW_WALLET_EXCEPTION_IF(true, error::wallet_internal_error,
      "Public key yielding at least one output wasn't found in the transaction extra");
  return crypto::null_pkey;
}

bool wallet::export_key_images(const std::string &filename) const
{
  std::vector<std::pair<crypto::key_image, crypto::signature>> ski = export_key_images();
  std::string magic(KEY_IMAGE_EXPORT_FILE_MAGIC, strlen(KEY_IMAGE_EXPORT_FILE_MAGIC));
  const cryptonote::account_public_address &keys = get_account().get_keys().m_account_address;

  std::string data;
  data += std::string((const char *)&keys.m_spend_public_key, sizeof(crypto::public_key));
  data += std::string((const char *)&keys.m_view_public_key, sizeof(crypto::public_key));
  for (const auto &i: ski)
  {
    data += std::string((const char *)&i.first, sizeof(crypto::key_image));
    data += std::string((const char *)&i.second, sizeof(crypto::signature));
  }

  // encrypt data, keep magic plaintext
  std::string ciphertext = encrypt_with_view_secret_key(data);
  return epee::file_io_utils::save_string_to_file(filename, magic + ciphertext);
}

//----------------------------------------------------------------------------------------------------
std::vector<std::pair<crypto::key_image, crypto::signature>> wallet::export_key_images() const
{
  std::vector<std::pair<crypto::key_image, crypto::signature>> ski;

  ski.reserve(m_transfers.size());
  for (size_t n = 0; n < m_transfers.size(); ++n)
  {
    const transfer_details &td = m_transfers[n];

    crypto::hash hash;
    crypto::cn_fast_hash(&td.m_key_image, sizeof(td.m_key_image), hash);

    // get ephemeral public key
    const cryptonote::tx_out &out = td.m_tx.vout[td.m_internal_output_index];
    THROW_WALLET_EXCEPTION_IF((!cryptonote::is_valid_transaction_output_type(out.target)), error::wallet_internal_error,
        "Output is not valid transaction output type");
    const auto pkey_opt = boost::apply_visitor(destination_public_key_visitor(), out.target);
    THROW_WALLET_EXCEPTION_IF((!pkey_opt), error::wallet_internal_error, "Output does not have valid key");
    const crypto::public_key &pkey = *pkey_opt;

    // get tx pub key
    std::vector<tx_extra_field> tx_extra_fields;
    if(!parse_tx_extra(td.m_tx.extra, tx_extra_fields))
    {
      // Extra may only be partially parsed, it's OK if tx_extra_fields contains public key
    }

    crypto::public_key tx_pub_key = get_tx_pub_key_from_received_outs(td);
    const std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(td.m_tx);

    // generate ephemeral secret key
    crypto::key_image ki = AUTO_VAL_INIT(ki);
    cryptonote::keypair in_ephemeral = AUTO_VAL_INIT(in_ephemeral);
    bool r = cryptonote::generate_key_image_helper(m_account.get_keys(), m_subaddresses, pkey, tx_pub_key, additional_tx_pub_keys, td.m_internal_output_index, in_ephemeral, ki, m_account.get_device());
    THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key image");

    THROW_WALLET_EXCEPTION_IF(td.m_key_image_known && !td.m_key_image_partial && ki != td.m_key_image,
        error::wallet_internal_error, "key_image generated not matched with cached key image");
    THROW_WALLET_EXCEPTION_IF(in_ephemeral.pub != pkey,
        error::wallet_internal_error, "key_image generated ephemeral public key not matched with output_key");

    // sign the key image with the output secret key
    crypto::signature signature = AUTO_VAL_INIT(signature);
    std::vector<const crypto::public_key*> key_ptrs;
    key_ptrs.push_back(&pkey);

    crypto::generate_ring_signature((const crypto::hash&)td.m_key_image, td.m_key_image, key_ptrs, in_ephemeral.sec, 0, &signature);

    ski.push_back(std::make_pair(td.m_key_image, signature));
  }
  return ski;
}

uint64_t wallet::import_key_images(const std::string &filename, uint64_t &spent, uint64_t &unspent)
{
  std::string data = AUTO_VAL_INIT(data);
  bool r = epee::file_io_utils::load_file_to_string(filename, data);

  THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, std::string(tr("failed to read file ")) + filename);

  const size_t magiclen = strlen(KEY_IMAGE_EXPORT_FILE_MAGIC);
  if (data.size() < magiclen || memcmp(data.data(), KEY_IMAGE_EXPORT_FILE_MAGIC, magiclen))
  {
    THROW_WALLET_EXCEPTION(error::wallet_internal_error, std::string("Bad key image export file magic in ") + filename);
  }

  try
  {
    data = decrypt_with_view_secret_key(std::string(data, magiclen));
  }
  catch (const std::exception &e)
  {
    THROW_WALLET_EXCEPTION(error::wallet_internal_error, std::string("Failed to decrypt ") + filename + ": " + e.what());
  }

  const size_t headerlen = 2 * sizeof(crypto::public_key);
  THROW_WALLET_EXCEPTION_IF(data.size() < headerlen, error::wallet_internal_error, std::string("Bad data size from file ") + filename);
  const crypto::public_key &public_spend_key = *(const crypto::public_key*)&data[0];
  const crypto::public_key &public_view_key = *(const crypto::public_key*)&data[sizeof(crypto::public_key)];
  const cryptonote::account_public_address &keys = get_account().get_keys().m_account_address;
  if (public_spend_key != keys.m_spend_public_key || public_view_key != keys.m_view_public_key)
  {
    THROW_WALLET_EXCEPTION(error::wallet_internal_error, std::string( "Key images from ") + filename + " are for a different account");
  }

  const size_t record_size = sizeof(crypto::key_image) + sizeof(crypto::signature);
  THROW_WALLET_EXCEPTION_IF((data.size() - headerlen) % record_size,
      error::wallet_internal_error, std::string("Bad data size from file ") + filename);
  size_t nki = (data.size() - headerlen) / record_size;

  std::vector<std::pair<crypto::key_image, crypto::signature>> ski;
  ski.reserve(nki);
  for (size_t n = 0; n < nki; ++n)
  {
    crypto::key_image key_image = *reinterpret_cast<const crypto::key_image*>(&data[headerlen + n * record_size]);
    crypto::signature signature = *reinterpret_cast<const crypto::signature*>(&data[headerlen + n * record_size + sizeof(crypto::key_image)]);

    ski.push_back(std::make_pair(key_image, signature));
  }

  return import_key_images(ski, spent, unspent);
}

//----------------------------------------------------------------------------------------------------
uint64_t wallet::import_key_images(const std::vector<std::pair<crypto::key_image, crypto::signature>> &signed_key_images, uint64_t &spent, uint64_t &unspent, bool check_spent)
{
  COMMAND_RPC_IS_KEY_IMAGE_SPENT::request req = AUTO_VAL_INIT(req);
  COMMAND_RPC_IS_KEY_IMAGE_SPENT::response daemon_resp = AUTO_VAL_INIT(daemon_resp);

  THROW_WALLET_EXCEPTION_IF(signed_key_images.size() > m_transfers.size(), error::wallet_internal_error,
      "The blockchain is out of date compared to the signed key images");

  if (signed_key_images.empty())
  {
    spent = 0;
    unspent = 0;
    return 0;
  }

  for (size_t n = 0; n < signed_key_images.size(); ++n)
  {
    const transfer_details &td = m_transfers[n];
    const crypto::key_image &key_image = signed_key_images[n].first;
    const crypto::signature &signature = signed_key_images[n].second;

    // get ephemeral public key
    const cryptonote::tx_out &out = td.m_tx.vout[td.m_internal_output_index];
    THROW_WALLET_EXCEPTION_IF((!cryptonote::is_valid_transaction_output_type(out.target)), error::wallet_internal_error,
      "Output is not valid transaction output type");
    const auto pkey_opt = boost::apply_visitor(destination_public_key_visitor(), out.target);
    THROW_WALLET_EXCEPTION_IF((!pkey_opt), error::wallet_internal_error, "Output does not have valid key");
    const crypto::public_key &pkey = *pkey_opt;

    std::vector<const crypto::public_key*> pkeys;
    pkeys.push_back(&pkey);
    THROW_WALLET_EXCEPTION_IF(!(rct::scalarmultKey(rct::ki2rct(key_image), rct::curveOrder()) == rct::identity()),
        error::wallet_internal_error, "Key image out of validity domain: input " + boost::lexical_cast<std::string>(n) + "/"
        + boost::lexical_cast<std::string>(signed_key_images.size()) + ", key image " + epee::string_tools::pod_to_hex(key_image));

    THROW_WALLET_EXCEPTION_IF(!crypto::check_ring_signature((const crypto::hash&)key_image, key_image, pkeys, &signature),
        error::wallet_internal_error, "Signature check failed: input " + boost::lexical_cast<std::string>(n) + "/"
        + boost::lexical_cast<std::string>(signed_key_images.size()) + ", key image " + epee::string_tools::pod_to_hex(key_image)
        + ", signature " + epee::string_tools::pod_to_hex(signature) + ", pubkey " + epee::string_tools::pod_to_hex(*pkeys[0]));

    req.key_images.push_back(epee::string_tools::pod_to_hex(key_image));
  }

  for (size_t n = 0; n < signed_key_images.size(); ++n)
  {
    m_transfers[n].m_key_image = signed_key_images[n].first;
    m_key_images[m_transfers[n].m_key_image] = n;
    m_transfers[n].m_key_image_known = true;
    m_transfers[n].m_key_image_partial = false;
  }

  if(check_spent)
  {
    m_daemon_rpc_mutex.lock();
    bool r = epee::net_utils::invoke_http_json("/is_key_image_spent", req, daemon_resp, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "is_key_image_spent");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "is_key_image_spent");
    THROW_WALLET_EXCEPTION_IF(daemon_resp.status != CORE_RPC_STATUS_OK, error::is_key_image_spent_error, daemon_resp.status);
    THROW_WALLET_EXCEPTION_IF(daemon_resp.spent_status.size() != signed_key_images.size(), error::wallet_internal_error,
      "daemon returned wrong response for is_key_image_spent, wrong amounts count = " +
      std::to_string(daemon_resp.spent_status.size()) + ", expected " +  std::to_string(signed_key_images.size()));
    for (size_t n = 0; n < daemon_resp.spent_status.size(); ++n)
    {
      transfer_details &td = m_transfers[n];
      td.m_spent = daemon_resp.spent_status[n] != COMMAND_RPC_IS_KEY_IMAGE_SPENT::UNSPENT;
    }
  }
  spent = 0;
  unspent = 0;
  std::unordered_set<crypto::hash> spent_txids;   // For each spent key image, search for a tx in m_transfers that uses it as input.
  std::vector<size_t> swept_transfers;            // If such a spending tx wasn't found in m_transfers, this means the spending tx
                                                  // was created by sweep_all, so we can't know the spent height and other detailed info.
  for(size_t i = 0; i < m_transfers.size(); ++i)
  {
    transfer_details &td = m_transfers[i];
    uint64_t amount = td.amount();
    if (td.m_spent)
      spent += amount;
    else
      unspent += amount;
    LOG_PRINT_L2("Transfer " << i << ": " << print_money(amount) << " (" << td.m_global_output_index << "): "
        << (td.m_spent ? "spent" : "unspent") << " (key image " << req.key_images[i] << ")");

    if (i < daemon_resp.spent_status.size() && daemon_resp.spent_status[i] == COMMAND_RPC_IS_KEY_IMAGE_SPENT::SPENT_IN_BLOCKCHAIN)
    {
      bool is_spent_tx_found = false;
      for (auto it = m_transfers.rbegin(); &(*it) != &td; ++it)
      {
        bool is_spent_tx = false;
        for(const cryptonote::txin_v& in : it->m_tx.vin)
        {
          if (cryptonote::is_valid_transaction_input_type(in,  it->m_tx.version))
          {
            auto k_image_opt = boost::apply_visitor(key_image_visitor(), in);
            if (k_image_opt && td.m_key_image == *k_image_opt)
            {
              is_spent_tx = true;
              break;
            }
          }
        }
        if (is_spent_tx)
        {
          is_spent_tx_found = true;
          spent_txids.insert(it->m_txid);
          break;
        }
      }

      if (!is_spent_tx_found)
        swept_transfers.push_back(i);
    }
  }
  MDEBUG("Total: " << print_money(spent) << " spent, " << print_money(unspent) << " unspent");

  if (check_spent)
  {
    // query outgoing txes
    COMMAND_RPC_GET_TRANSACTIONS::request gettxs_req = AUTO_VAL_INIT(gettxs_req);
    COMMAND_RPC_GET_TRANSACTIONS::response gettxs_res = AUTO_VAL_INIT(gettxs_res);
    gettxs_req.decode_as_json = false;
    gettxs_req.prune = false;
    for (const crypto::hash& spent_txid : spent_txids)
      gettxs_req.txs_hashes.push_back(epee::string_tools::pod_to_hex(spent_txid));
    m_daemon_rpc_mutex.lock();
    bool r = epee::net_utils::invoke_http_json("/gettransactions", gettxs_req, gettxs_res, m_http_client, rpc_timeout);
    m_daemon_rpc_mutex.unlock();
    THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "gettransactions");
    THROW_WALLET_EXCEPTION_IF(gettxs_res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "gettransactions");
    THROW_WALLET_EXCEPTION_IF(gettxs_res.txs.size() != spent_txids.size(), error::wallet_internal_error,
      "daemon returned wrong response for gettransactions, wrong count = " + std::to_string(gettxs_res.txs.size()) + ", expected " + std::to_string(spent_txids.size()));

    // process each outgoing tx
    auto spent_txid = spent_txids.begin();
    hw::device &hwdev =  m_account.get_device();
    for (const COMMAND_RPC_GET_TRANSACTIONS::entry& e : gettxs_res.txs)
    {
      THROW_WALLET_EXCEPTION_IF(e.in_pool, error::wallet_internal_error, "spent tx isn't supposed to be in txpool");

      // parse tx
      cryptonote::blobdata bd;
      THROW_WALLET_EXCEPTION_IF(!epee::string_tools::parse_hexstr_to_binbuff(e.as_hex, bd), error::wallet_internal_error, "parse_hexstr_to_binbuff failed");
      cryptonote::transaction spent_tx = AUTO_VAL_INIT(spent_tx);
      crypto::hash spnet_txid_parsed, spent_txid_prefix;
      THROW_WALLET_EXCEPTION_IF(!cryptonote::parse_and_validate_tx_from_blob(bd, spent_tx, spnet_txid_parsed, spent_txid_prefix), error::wallet_internal_error, "parse_and_validate_tx_from_blob failed");
      THROW_WALLET_EXCEPTION_IF(*spent_txid != spnet_txid_parsed, error::wallet_internal_error, "parsed txid mismatch");

      // get received (change) amount
      uint64_t tx_money_got_in_outs = 0;
      uint64_t tx_tokens_got_in_outs = 0;
      const cryptonote::account_keys& keys = m_account.get_keys();
      const crypto::public_key tx_pub_key = get_tx_pub_key_from_extra(spent_tx);
      crypto::key_derivation derivation;
      bool r = hwdev.generate_key_derivation(tx_pub_key, keys.m_view_secret_key, derivation);
      THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key derivation");
      const std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(spent_tx);
      std::vector<crypto::key_derivation> additional_derivations;
      for (size_t i = 0; i < additional_tx_pub_keys.size(); ++i)
      {
        additional_derivations.push_back({});
        r = hwdev.generate_key_derivation(additional_tx_pub_keys[i], keys.m_view_secret_key, additional_derivations.back());
        THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key derivation");
      }
      size_t output_index = 0;
      for (const cryptonote::tx_out& out : spent_tx.vout)
      {
        tx_scan_info_t tx_scan_info = AUTO_VAL_INIT(tx_scan_info);
        check_acc_out_precomp(out, derivation, additional_derivations, output_index, tx_scan_info);
        THROW_WALLET_EXCEPTION_IF(tx_scan_info.error, error::wallet_internal_error, "check_acc_out_precomp failed");
        if (tx_scan_info.received)
        {
          tx_money_got_in_outs += tx_scan_info.money_transfered;
          tx_tokens_got_in_outs += tx_scan_info.output_type == tx_out_type::out_token ? tx_scan_info.token_transfered : 0;
        }
        ++output_index;
      }

      // get spent amount
      uint64_t tx_money_spent_in_ins = 0;
      uint64_t tx_tokens_spent_in_ins = 0;
      uint32_t subaddr_account = (uint32_t)-1;
      std::set<uint32_t> subaddr_indices;
      for (const cryptonote::txin_v& in : spent_tx.vin)
      {
        if (!cryptonote::is_valid_transaction_input_type(in, spent_tx.version))
          continue;

        const auto k_image_opt = boost::apply_visitor(key_image_visitor(), in);
        if (!k_image_opt)
          continue;

        auto it = m_key_images.find(*k_image_opt);
        if (it != m_key_images.end())
        {
          const transfer_details& td = m_transfers[it->second];
          uint64_t value_amount = *boost::apply_visitor(amount_visitor(), in);
          if (value_amount > 0)
          {
            THROW_WALLET_EXCEPTION_IF((!td.m_token_transfer && value_amount != td.amount()), error::wallet_internal_error,
                                      std::string("Inconsistent amount in tx input: got ") + print_money(value_amount) +
                                      std::string(", expected ") + print_money(td.amount()));
            THROW_WALLET_EXCEPTION_IF((td.m_token_transfer && value_amount != td.token_amount()), error::wallet_internal_error,
                                      std::string("Inconsistent token amount in tx input: got ") + print_money(value_amount) +
                                      std::string(", expected ") + print_money(td.token_amount()));
          }
          if (td.m_token_transfer && td.m_output_type == tx_out_type::out_token)
          {
            value_amount = td.token_amount();
            tx_tokens_spent_in_ins += value_amount;
            LOG_PRINT_L0("Spent tokens: " << print_money(value_amount) << ", with tx: " << *spent_txid);
          }
          else if (td.m_output_type == tx_out_type::out_cash)
          {
            value_amount = td.amount();
            tx_money_spent_in_ins += value_amount;
            LOG_PRINT_L0("Spent money: " << print_money(value_amount) << ", with tx: " << *spent_txid);
          }

          set_spent(it->second, e.block_height);

          if (m_callback && !td.m_token_transfer)
            m_callback->on_money_spent(e.block_height, *spent_txid, spent_tx, value_amount, spent_tx, td.m_subaddr_index);
          if (m_callback && td.m_token_transfer)
            m_callback->on_tokens_spent(e.block_height, *spent_txid, spent_tx, value_amount, spent_tx, td.m_subaddr_index);

          if (subaddr_account != (uint32_t)-1 && subaddr_account != td.m_subaddr_index.major)
            LOG_PRINT_L0("WARNING: This tx spends outputs received by different subaddress accounts, which isn't supposed to happen");
          subaddr_account = td.m_subaddr_index.major;
          subaddr_indices.insert(td.m_subaddr_index.minor);
        }
      }

      // create outgoing payment
      process_outgoing(*spent_txid, spent_tx, e.block_height, e.block_timestamp, tx_money_spent_in_ins, tx_tokens_spent_in_ins, tx_money_got_in_outs, tx_tokens_got_in_outs, subaddr_account, subaddr_indices);

      // erase corresponding incoming payment
      for (auto j = m_payments.begin(); j != m_payments.end(); ++j)
      {
        if (j->second.m_tx_hash == *spent_txid)
        {
          m_payments.erase(j);
          break;
        }
      }

      ++spent_txid;
    }

    for (size_t n : swept_transfers)
    {
      const transfer_details& td = m_transfers[n];
      confirmed_transfer_details pd = AUTO_VAL_INIT(pd);
      pd.m_change = (uint64_t)-1;                             // change is unknown
      pd.m_amount_in = pd.m_amount_out = td.amount();         // fee is unknown
      pd.m_token_amount_in = pd.m_token_amount_out = td.token_amount();
      pd.m_output_type = td.m_output_type;
      std::string err;
      pd.m_block_height = get_daemon_blockchain_height(err);  // spent block height is unknown, so hypothetically set to the highest
      crypto::hash spent_txid = crypto::rand<crypto::hash>(); // spent txid is unknown, so hypothetically set to random
      m_confirmed_txs.insert(std::make_pair(spent_txid, pd));
    }
  }

  return m_transfers[signed_key_images.size() - 1].m_block_height;
}
wallet::payment_container wallet::export_payments() const
{
  payment_container payments;
  for (auto const &p : m_payments)
  {
    payments.emplace(p);
  }
  return payments;
}
void wallet::import_payments(const payment_container &payments)
{
  m_payments.clear();
  for (auto const &p : payments)
  {
    m_payments.emplace(p);
  }
}
void wallet::import_payments_out(const std::list<std::pair<crypto::hash,wallet::confirmed_transfer_details>> &confirmed_payments)
{
  m_confirmed_txs.clear();
  for (auto const &p : confirmed_payments)
  {
    m_confirmed_txs.emplace(p);
  }
}

std::tuple<size_t,crypto::hash,std::vector<crypto::hash>> wallet::export_blockchain() const
{
  std::tuple<size_t, crypto::hash, std::vector<crypto::hash>> bc;
  std::get<0>(bc) = m_blockchain.offset();
  std::get<1>(bc) = m_blockchain.empty() ? crypto::null_hash: m_blockchain.genesis();
  for (size_t n = m_blockchain.offset(); n < m_blockchain.size(); ++n)
  {
    std::get<2>(bc).push_back(m_blockchain[n]);
  }
  return bc;
}

void wallet::import_blockchain(const std::tuple<size_t, crypto::hash, std::vector<crypto::hash>> &bc)
{
  m_blockchain.clear();
  if (std::get<0>(bc))
  {
    for (size_t n = std::get<0>(bc); n > 0; --n)
      m_blockchain.push_back(std::get<1>(bc));
    m_blockchain.trim(std::get<0>(bc));
  }
  for (auto const &b : std::get<2>(bc))
  {
    m_blockchain.push_back(b);
  }
  cryptonote::block genesis;
  generate_genesis(genesis);
  crypto::hash genesis_hash = get_block_hash(genesis);
  check_genesis(genesis_hash);
  m_local_bc_height = m_blockchain.size();
}
//----------------------------------------------------------------------------------------------------
std::vector<tools::wallet::transfer_details> wallet::export_outputs() const
{
  std::vector<tools::wallet::transfer_details> outs;

  outs.reserve(m_transfers.size());
  for (size_t n = 0; n < m_transfers.size(); ++n)
  {
    const transfer_details &td = m_transfers[n];

    outs.push_back(td);
  }

  return outs;
}
//----------------------------------------------------------------------------------------------------
size_t wallet::import_outputs(const std::vector<tools::wallet::transfer_details> &outputs)
{
  m_transfers.clear();
  m_transfers.reserve(outputs.size());
  for (size_t i = 0; i < outputs.size(); ++i)
  {
    transfer_details td = outputs[i];

    // the hot wallet wouldn't have known about key images (except if we already exported them)
    cryptonote::keypair in_ephemeral = AUTO_VAL_INIT(in_ephemeral);

    THROW_WALLET_EXCEPTION_IF(td.m_tx.vout.empty(), error::wallet_internal_error, "tx with no outputs at index " + boost::lexical_cast<std::string>(i));
    crypto::public_key tx_pub_key = get_tx_pub_key_from_received_outs(td);
    const std::vector<crypto::public_key> additional_tx_pub_keys = get_additional_tx_pub_keys_from_extra(td.m_tx);

    //const crypto::public_key& out_key = boost::get<cryptonote::txout_to_key>(td.m_tx.vout[td.m_internal_output_index].target).key;
    const auto out_key_opt = boost::apply_visitor(destination_public_key_visitor(), td.m_tx.vout[td.m_internal_output_index].target);
    THROW_WALLET_EXCEPTION_IF((!out_key_opt), error::wallet_internal_error, "tx with output which does not have valid key");
    const crypto::public_key &out_key = *out_key_opt;


    bool r = cryptonote::generate_key_image_helper(m_account.get_keys(), m_subaddresses, out_key, tx_pub_key, additional_tx_pub_keys, td.m_internal_output_index, in_ephemeral, td.m_key_image, m_account.get_device());
    THROW_WALLET_EXCEPTION_IF(!r, error::wallet_internal_error, "Failed to generate key image");
    expand_subaddresses(td.m_subaddr_index);
    td.m_key_image_known = true;
    td.m_key_image_partial = false;
    THROW_WALLET_EXCEPTION_IF(in_ephemeral.pub != out_key,
        error::wallet_internal_error, "key_image generated ephemeral public key not matched with output_key at index " + boost::lexical_cast<std::string>(i));

    m_key_images[td.m_key_image] = m_transfers.size();
    m_pub_keys[td.get_public_key()] = m_transfers.size();
    m_transfers.push_back(td);
  }

  return m_transfers.size();
}
//----------------------------------------------------------------------------------------------------
std::string wallet::encrypt(const std::string &plaintext, const crypto::secret_key &skey, bool authenticated) const
{
  crypto::chacha_key key;
  crypto::generate_chacha_key(&skey, sizeof(skey), key);
  std::string ciphertext;
  crypto::chacha_iv iv = crypto::rand<crypto::chacha_iv>();
  ciphertext.resize(plaintext.size() + sizeof(iv) + (authenticated ? sizeof(crypto::signature) : 0));
  crypto::chacha20(plaintext.data(), plaintext.size(), key, iv, &ciphertext[sizeof(iv)]);
  memcpy(&ciphertext[0], &iv, sizeof(iv));
  if (authenticated)
  {
    crypto::hash hash;
    crypto::cn_fast_hash(ciphertext.data(), ciphertext.size() - sizeof(signature), hash);
    crypto::public_key pkey;
    crypto::secret_key_to_public_key(skey, pkey);
    crypto::signature &signature = *(crypto::signature*)&ciphertext[ciphertext.size() - sizeof(crypto::signature)];
    crypto::generate_signature(hash, pkey, skey, signature);
  }
  return ciphertext;
}
//----------------------------------------------------------------------------------------------------
std::string wallet::encrypt_with_view_secret_key(const std::string &plaintext, bool authenticated) const
{
  return encrypt(plaintext, get_account().get_keys().m_view_secret_key, authenticated);
}
//----------------------------------------------------------------------------------------------------
std::string wallet::decrypt(const std::string &ciphertext, const crypto::secret_key &skey, bool authenticated) const
{
  const size_t prefix_size = sizeof(chacha_iv) + (authenticated ? sizeof(crypto::signature) : 0);
  THROW_WALLET_EXCEPTION_IF(ciphertext.size() < prefix_size,
    error::wallet_internal_error, "Unexpected ciphertext size");

  crypto::chacha_key key;
  crypto::generate_chacha_key(&skey, sizeof(skey), key);
  const crypto::chacha_iv &iv = *(const crypto::chacha_iv*)&ciphertext[0];
  std::string plaintext;
  plaintext.resize(ciphertext.size() - prefix_size);
  if (authenticated)
  {
    crypto::hash hash;
    crypto::cn_fast_hash(ciphertext.data(), ciphertext.size() - sizeof(signature), hash);
    crypto::public_key pkey;
    crypto::secret_key_to_public_key(skey, pkey);
    const crypto::signature &signature = *(const crypto::signature*)&ciphertext[ciphertext.size() - sizeof(crypto::signature)];
    THROW_WALLET_EXCEPTION_IF(!crypto::check_signature(hash, pkey, signature),
      error::wallet_internal_error, "Failed to authenticate ciphertext");
  }
  crypto::chacha20(ciphertext.data() + sizeof(iv), ciphertext.size() - prefix_size, key, iv, &plaintext[0]);
  return plaintext;
}
//----------------------------------------------------------------------------------------------------
std::string wallet::decrypt_with_view_secret_key(const std::string &ciphertext, bool authenticated) const
{
  return decrypt(ciphertext, get_account().get_keys().m_view_secret_key, authenticated);
}
//----------------------------------------------------------------------------------------------------
std::string wallet::make_uri(const std::string &address, const std::string &payment_id, uint64_t amount, uint64_t token_amount, const std::string &tx_description, const std::string &recipient_name, std::string &error) const
{
  cryptonote::address_parse_info info;
  if(!get_account_address_from_str(info, nettype(), address))
  {
    error = std::string("wrong address: ") + address;
    return std::string();
  }

  // we want only one payment id
  if (info.has_payment_id && !payment_id.empty())
  {
    error = "A single payment id is allowed";
    return std::string();
  }

  if (!payment_id.empty())
  {
    crypto::hash pid32;
    crypto::hash8 pid8;
    if (!wallet::parse_long_payment_id(payment_id, pid32) && !wallet::parse_short_payment_id(payment_id, pid8))
    {
      error = "Invalid payment id";
      return std::string();
    }
  }

  std::string uri = "safex:" + address;
  unsigned int n_fields = 0;

  if (!payment_id.empty())
  {
    uri += (n_fields++ ? "&" : "?") + std::string("tx_payment_id=") + payment_id;
  }

  if (amount > 0)
  {
    // URI encoded amount is in decimal units, not atomic units
    uri += (n_fields++ ? "&" : "?") + std::string("tx_cash_amount=") + cryptonote::print_money(amount);
  }

  if (token_amount > 0)
  {
    // URI encoded amount is in decimal units, not atomic units
    uri += (n_fields++ ? "&" : "?") + std::string("tx_token_amount=") + cryptonote::print_money(token_amount);
  }


  if (!recipient_name.empty())
  {
    uri += (n_fields++ ? "&" : "?") + std::string("recipient_name=") + epee::net_utils::conver_to_url_format(recipient_name);
  }

  if (!tx_description.empty())
  {
    uri += (n_fields++ ? "&" : "?") + std::string("tx_description=") + epee::net_utils::conver_to_url_format(tx_description);
  }

  return uri;
}
//----------------------------------------------------------------------------------------------------
bool wallet::parse_uri(const std::string &uri, std::string &address, std::string &payment_id, uint64_t &amount, uint64_t& token_amount, std::string &tx_description, std::string &recipient_name, std::vector<std::string> &unknown_parameters, std::string &error)
{
  if (uri.substr(0, 6) != "safex:")
  {
    error = std::string("URI has wrong scheme (expected \"safex:\"): ") + uri;
    return false;
  }

  std::string remainder = uri.substr(6);
  const char *ptr = strchr(remainder.c_str(), '?');
  address = ptr ? remainder.substr(0, ptr-remainder.c_str()) : remainder;

  cryptonote::address_parse_info info;
  if(!get_account_address_from_str(info, nettype(), address))
  {
    error = std::string("URI has wrong address: ") + address;
    return false;
  }
  if (!strchr(remainder.c_str(), '?'))
    return true;

  std::vector<std::string> arguments;
  std::string body = remainder.substr(address.size() + 1);
  if (body.empty())
    return true;
  boost::split(arguments, body, boost::is_any_of("&"));
  std::set<std::string> have_arg;
  for (const auto &arg: arguments)
  {
    std::vector<std::string> kv;
    boost::split(kv, arg, boost::is_any_of("="));
    if (kv.size() != 2)
    {
      error = std::string("URI has wrong parameter: ") + arg;
      return false;
    }
    if (have_arg.find(kv[0]) != have_arg.end())
    {
      error = std::string("URI has more than one instance of " + kv[0]);
      return false;
    }
    have_arg.insert(kv[0]);

    if (kv[0] == "tx_cash_amount")
    {
      amount = 0;
      if (!cryptonote::parse_amount(amount, kv[1]))
      {
        error = std::string("URI has invalid cash_amount: ") + kv[1];
        return false;
      }
    }
    else if (kv[0] == "tx_token_amount")
    {
      amount = 0;
      if (!cryptonote::parse_amount(token_amount, kv[1]))
      {
        error = std::string("URI has invalid token_amount: ") + kv[1];
        return false;
      }
    }
    else if (kv[0] == "tx_payment_id")
    {
      if (info.has_payment_id)
      {
        error = "Separate payment id given with an integrated address";
        return false;
      }
      crypto::hash hash;
      crypto::hash8 hash8;
      if (!wallet::parse_long_payment_id(kv[1], hash) && !wallet::parse_short_payment_id(kv[1], hash8))
      {
        error = "Invalid payment id: " + kv[1];
        return false;
      }
      payment_id = kv[1];
    }
    else if (kv[0] == "recipient_name")
    {
      recipient_name = epee::net_utils::convert_from_url_format(kv[1]);
    }
    else if (kv[0] == "tx_description")
    {
      tx_description = epee::net_utils::convert_from_url_format(kv[1]);
    }
    else
    {
      unknown_parameters.push_back(arg);
    }
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::get_blockchain_height_by_date(uint16_t year, uint8_t month, uint8_t day)
{
  uint32_t version;
  if (!check_connection(&version))
  {
    throw std::runtime_error("failed to connect to daemon: " + get_daemon_address());
  }
  if (version < MAKE_CORE_RPC_VERSION(1, 6))
  {
    throw std::runtime_error("this function requires RPC version 1.6 or higher");
  }
  std::tm date = { 0, 0, 0, 0, 0, 0, 0, 0 };
  date.tm_year = year - 1900;
  date.tm_mon  = month - 1;
  date.tm_mday = day;
  if (date.tm_mon < 0 || 11 < date.tm_mon || date.tm_mday < 1 || 31 < date.tm_mday)
  {
    throw std::runtime_error("month or day out of range");
  }
  uint64_t timestamp_target = std::mktime(&date);
  std::string err;
  uint64_t height_min = 0;
  uint64_t height_max = get_daemon_blockchain_height(err) - 1;
  if (!err.empty())
  {
    throw std::runtime_error("failed to get blockchain height");
  }
  while (true)
  {
    COMMAND_RPC_GET_BLOCKS_BY_HEIGHT::request req;
    COMMAND_RPC_GET_BLOCKS_BY_HEIGHT::response res;
    uint64_t height_mid = (height_min + height_max) / 2;
    req.heights =
    {
      height_min,
      height_mid,
      height_max
    };
    bool r = net_utils::invoke_http_bin("/getblocks_by_height.bin", req, res, m_http_client, rpc_timeout);
    if (!r || res.status != CORE_RPC_STATUS_OK)
    {
      std::ostringstream oss;
      oss << "failed to get blocks by heights: ";
      for (auto height : req.heights)
        oss << height << ' ';
      oss << endl << "reason: ";
      if (!r)
        oss << "possibly lost connection to daemon";
      else if (res.status == CORE_RPC_STATUS_BUSY)
        oss << "daemon is busy";
      else
        oss << res.status;
      throw std::runtime_error(oss.str());
    }
    cryptonote::block blk_min, blk_mid, blk_max;
    if (res.blocks.size() < 3) throw std::runtime_error("Not enough blocks returned from daemon");
    if (!parse_and_validate_block_from_blob(res.blocks[0].block, blk_min)) throw std::runtime_error("failed to parse blob at height " + std::to_string(height_min));
    if (!parse_and_validate_block_from_blob(res.blocks[1].block, blk_mid)) throw std::runtime_error("failed to parse blob at height " + std::to_string(height_mid));
    if (!parse_and_validate_block_from_blob(res.blocks[2].block, blk_max)) throw std::runtime_error("failed to parse blob at height " + std::to_string(height_max));
    uint64_t timestamp_min = blk_min.timestamp;
    uint64_t timestamp_mid = blk_mid.timestamp;
    uint64_t timestamp_max = blk_max.timestamp;
    if (!(timestamp_min <= timestamp_mid && timestamp_mid <= timestamp_max))
    {
      // the timestamps are not in the chronological order.
      // assuming they're sufficiently close to each other, simply return the smallest height
      return std::min({height_min, height_mid, height_max});
    }
    if (timestamp_target > timestamp_max)
    {
      throw std::runtime_error("specified date is in the future");
    }
    if (timestamp_target <= timestamp_min + 2 * 24 * 60 * 60)   // two days of "buffer" period
    {
      return height_min;
    }
    if (timestamp_target <= timestamp_mid)
      height_max = height_mid;
    else
      height_min = height_mid;
    if (height_max - height_min <= 2 * 24 * 30)        // don't divide the height range finer than two days
    {
      return height_min;
    }
  }
}
//----------------------------------------------------------------------------------------------------
bool wallet::is_synced() const
{
  uint64_t height;
  boost::optional<std::string> result = m_node_rpc_proxy.get_target_height(height);
  if (result && *result != CORE_RPC_STATUS_OK)
    return false;
  return get_blockchain_current_height() >= height;
}
//----------------------------------------------------------------------------------------------------
std::vector<std::pair<uint64_t, uint64_t>> wallet::estimate_backlog(const std::vector<std::pair<double, double>> &fee_levels)
{
  for (const auto &fee_level: fee_levels)
  {
    THROW_WALLET_EXCEPTION_IF(fee_level.first == 0.0, error::wallet_internal_error, "Invalid 0 fee");
    THROW_WALLET_EXCEPTION_IF(fee_level.second == 0.0, error::wallet_internal_error, "Invalid 0 fee");
  }

  // get txpool backlog
  cryptonote::COMMAND_RPC_GET_TRANSACTION_POOL_BACKLOG::request req = AUTO_VAL_INIT(req);
  cryptonote::COMMAND_RPC_GET_TRANSACTION_POOL_BACKLOG::response res = AUTO_VAL_INIT(res);
  m_daemon_rpc_mutex.lock();
  bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_txpool_backlog", req, res, m_http_client, rpc_timeout);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "Failed to connect to daemon");
  THROW_WALLET_EXCEPTION_IF(res.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_txpool_backlog");
  THROW_WALLET_EXCEPTION_IF(res.status != CORE_RPC_STATUS_OK, error::get_tx_pool_error);

  cryptonote::COMMAND_RPC_GET_INFO::request req_t = AUTO_VAL_INIT(req_t);
  cryptonote::COMMAND_RPC_GET_INFO::response resp_t = AUTO_VAL_INIT(resp_t);
  m_daemon_rpc_mutex.lock();
  r = net_utils::invoke_http_json_rpc("/json_rpc", "get_info", req_t, resp_t, m_http_client);
  m_daemon_rpc_mutex.unlock();
  THROW_WALLET_EXCEPTION_IF(!r, error::no_connection_to_daemon, "get_info");
  THROW_WALLET_EXCEPTION_IF(resp_t.status == CORE_RPC_STATUS_BUSY, error::daemon_busy, "get_info");
  THROW_WALLET_EXCEPTION_IF(resp_t.status != CORE_RPC_STATUS_OK, error::get_tx_pool_error);
  uint64_t full_reward_zone = resp_t.block_size_limit / 2;

  std::vector<std::pair<uint64_t, uint64_t>> blocks;
  for (const auto &fee_level: fee_levels)
  {
    const double our_fee_byte_min = fee_level.first;
    const double our_fee_byte_max = fee_level.second;
    uint64_t priority_size_min = 0, priority_size_max = 0;
    for (const auto &i: res.backlog)
    {
      if (i.blob_size == 0)
      {
        MWARNING("Got 0 sized blob from txpool, ignored");
        continue;
      }
      double this_fee_byte = i.fee / (double)i.blob_size;
      if (this_fee_byte >= our_fee_byte_min)
        priority_size_min += i.blob_size;
      if (this_fee_byte >= our_fee_byte_max)
        priority_size_max += i.blob_size;
    }

    uint64_t nblocks_min = priority_size_min / full_reward_zone;
    uint64_t nblocks_max = priority_size_max / full_reward_zone;
    MDEBUG("estimate_backlog: priority_size " << priority_size_min << " - " << priority_size_max << " for "
        << our_fee_byte_min << " - " << our_fee_byte_max << " piconero byte fee, "
        << nblocks_min << " - " << nblocks_max << " blocks at block size " << full_reward_zone);
    blocks.push_back(std::make_pair(nblocks_min, nblocks_max));
  }
  return blocks;
}
//----------------------------------------------------------------------------------------------------
std::vector<std::pair<uint64_t, uint64_t>> wallet::estimate_backlog(uint64_t min_blob_size, uint64_t max_blob_size, const std::vector<uint64_t> &fees)
{
  THROW_WALLET_EXCEPTION_IF(min_blob_size == 0, error::wallet_internal_error, "Invalid 0 fee");
  THROW_WALLET_EXCEPTION_IF(max_blob_size == 0, error::wallet_internal_error, "Invalid 0 fee");
  for (uint64_t fee: fees)
  {
    THROW_WALLET_EXCEPTION_IF(fee == 0, error::wallet_internal_error, "Invalid 0 fee");
  }
  std::vector<std::pair<double, double>> fee_levels;
  for (uint64_t fee: fees)
  {
    double our_fee_byte_min = fee / (double)min_blob_size, our_fee_byte_max = fee / (double)max_blob_size;
    fee_levels.emplace_back(our_fee_byte_min, our_fee_byte_max);
  }
  return estimate_backlog(fee_levels);
}
//----------------------------------------------------------------------------------------------------
uint64_t wallet::get_segregation_fork_height() const
{
  if (m_nettype == TESTNET)
    return TESTNET_SEGREGATION_FORK_HEIGHT;
  if (m_nettype == STAGENET)
    return STAGENET_SEGREGATION_FORK_HEIGHT;
  THROW_WALLET_EXCEPTION_IF(m_nettype != MAINNET, tools::error::wallet_internal_error, "Invalid network type");

  if (m_segregation_height > 0)
    return m_segregation_height;

  static const bool use_dns = false;
  if (use_dns)
  {
    //todo Setup this for Safex
    static const std::vector<std::string> dns_urls = {
        "segheights.safex.org",
    };

    const uint64_t current_height = get_blockchain_current_height();
    uint64_t best_diff = std::numeric_limits<uint64_t>::max(), best_height = 0;
    std::vector<std::string> records;
    if (tools::dns_utils::load_txt_records_from_dns(records, dns_urls))
    {
      for (const auto& record : records)
      {
        std::vector<std::string> fields;
        boost::split(fields, record, boost::is_any_of(":"));
        if (fields.size() != 2)
          continue;
        uint64_t height;
        if (!string_tools::get_xtype_from_string(height, fields[1]))
          continue;

        MINFO("Found segregation height via DNS: " << fields[0] << " fork height at " << height);
        uint64_t diff = height > current_height ? height - current_height : current_height - height;
        if (diff < best_diff)
        {
          best_diff = diff;
          best_height = height;
        }
      }
      if (best_height)
        return best_height;
    }
  }
  return SEGREGATION_FORK_HEIGHT;
}



//----------------------------------------------------------------------------------------------------
void wallet::generate_genesis(cryptonote::block& b) const {
  if (m_nettype == TESTNET)
  {
    cryptonote::generate_genesis_block(b, config::testnet::GENESIS_TX, config::testnet::GENESIS_NONCE);
  }
  else if (m_nettype == STAGENET)
  {
    cryptonote::generate_genesis_block(b, config::stagenet::GENESIS_TX, config::stagenet::GENESIS_NONCE);
  }
  else
  {
    cryptonote::generate_genesis_block(b, config::GENESIS_TX, config::GENESIS_NONCE);
  }
}

crypto::public_key wallet::get_migration_verification_public_key() const
{
  crypto::public_key public_key;
  THROW_WALLET_EXCEPTION_IF(!cryptonote::get_migration_verification_public_key(nettype(), public_key),
                            tools::error::invalid_nettype);
  return public_key;
}

}
