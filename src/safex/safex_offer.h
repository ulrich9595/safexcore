//
// Created by amarko on 22.7.19..
//

#ifndef SAFEX_SAFEX_OFFER_H
#define SAFEX_SAFEX_OFFER_H


#include <string>
#include <cryptonote_basic/cryptonote_basic.h>

#include "device/device.hpp"
#include "crypto/crypto.h"
#include "serialization/keyvalue_serialization.h"
#include "cryptonote_basic/cryptonote_format_utils.h"


#include "safex_core.h"

#undef SAFEX_DEFAULT_LOG_CATEGORY
#define SAFEX_DEFAULT_LOG_CATEGORY "safex_offer"

namespace safex
{


  struct safex_offer
  {
    public:
      safex_offer(): title{}, quantity{}, price{}, description{},  active{false}, shipping{}, offer_id{0},  seller{} {

      }

      safex_offer(const std::string &_title, const uint64_t _quantity, const uint64_t _price, const std::vector<uint8_t> &_description,
                  crypto::hash _id, std::string seller_username, bool _active = true , const cryptonote::account_public_address& _seller_address = {}, const crypto::secret_key& view_key = {}):title{_title},quantity{_quantity},price{_price},
                                                            description{_description},offer_id{_id},seller{seller_username},active{_active},seller_private_view_key{view_key},seller_address{_seller_address}
      {
      }


      safex_offer(const std::string &_title, const uint64_t _quantity, const uint64_t _price, const std::string& _description,
               std::string seller_username, const crypto::secret_key& view_key, const cryptonote::account_public_address& _seller_address = {}):
              title{_title}, quantity{_quantity}, price{_price}, active{true}, shipping{}, seller{seller_username},seller_private_view_key{view_key},seller_address{_seller_address} {

          description = std::vector<uint8_t>(_description.begin(),_description.end());
          offer_id = create_offer_id(seller_username);

      }

    BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(title)
        KV_SERIALIZE(quantity)
        KV_SERIALIZE(price)
        KV_SERIALIZE(description)
        KV_SERIALIZE(active)
        KV_SERIALIZE(shipping)
        KV_SERIALIZE(offer_id)
        KV_SERIALIZE(seller)
        KV_SERIALIZE(seller_private_view_key)
        KV_SERIALIZE(seller_address)
      END_KV_SERIALIZE_MAP()

      BEGIN_SERIALIZE_OBJECT()
        FIELD(title)
        VARINT_FIELD(quantity)
        FIELD(price)
        FIELD(description)
        FIELD(active)
        FIELD(shipping)
        FIELD(offer_id)
        FIELD(seller)
        FIELD(seller_private_view_key)
        FIELD(seller_address)
      END_SERIALIZE()

      template<class t_archive>
      inline void serialize(t_archive &a, const unsigned int /*ver*/)
      {
        a & title;
        a & quantity;
        a & price;
        a & description;
        a & active;
        a & shipping;
        a & offer_id;
        a & seller;
        a & seller_private_view_key;
        a & seller_address;
      }


      std::string title; //title of the offer
      uint64_t quantity;
      uint64_t price;
      std::vector<uint8_t> description; //description of offer, JSON or other format TBD.
      bool active; //is offer active
      std::vector<uint8_t> shipping;
      crypto::hash offer_id; //unique id of the offer
      std::string seller; // username of the seller
      crypto::secret_key seller_private_view_key;
      cryptonote::account_public_address seller_address;

  private:
      crypto::hash create_offer_id(std::string& username);

  };
}


#endif //SAFEX_SAFEX_OFFER_H
