/* (c) 2016, 2017 DECENT Services. For details refers to LICENSE.txt */
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
#include <graphene/chain/asset_evaluator.hpp>
#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/account_object.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/hardfork.hpp>

#include <functional>

namespace graphene { namespace chain {

void_result asset_create_evaluator::do_evaluate( const asset_create_operation& op )
{ try {

   database& d = db();

   const auto& chain_parameters = d.get_global_properties().parameters;

   auto& asset_indx = d.get_index_type<asset_index>().indices().get<by_symbol>();
   auto asset_symbol_itr = asset_indx.find( op.symbol );
   FC_ASSERT( asset_symbol_itr == asset_indx.end() );

   auto dotpos = op.symbol.rfind( '.' );
   if( dotpos != std::string::npos )
   {
      auto prefix = op.symbol.substr( 0, dotpos );
      auto asset_symbol_itr = asset_indx.find( prefix );
      FC_ASSERT( asset_symbol_itr != asset_indx.end(), "Asset ${s} may only be created by issuer of ${p}, but ${p} has not been registered",
            ("s",op.symbol)("p",prefix) );
      FC_ASSERT( asset_symbol_itr->issuer == op.issuer, "Asset ${s} may only be created by issuer of ${p}, ${i}",
            ("s",op.symbol)("p",prefix)("i", op.issuer(d).name) );
   }

   core_fee_paid -= core_fee_paid.value/2;

   if( op.monitored_asset_opts.valid() )
   {
      FC_ASSERT( op.monitored_asset_opts->feed_lifetime_sec > chain_parameters.block_interval );
      FC_ASSERT( op.options.max_supply == 0, );
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

object_id_type asset_create_evaluator::do_apply( const asset_create_operation& op )
{ try {
   const asset_dynamic_data_object& dyn_asset =
      db().create<asset_dynamic_data_object>( [&]( asset_dynamic_data_object& a ) {
         a.current_supply = 0;
         a.fee_pool = core_fee_paid; //op.calculate_fee(db().current_fee_schedule()).value / 2;
      });

   auto next_asset_id = db().get_index_type<asset_index>().get_next_id();

   const asset_object& new_asset =
     db().create<asset_object>( [&]( asset_object& a ) {
         a.issuer = op.issuer;
         a.symbol = op.symbol;
         a.precision = op.precision;
         a.description = op.description;
         a.options = op.options;
         a.monitored_asset_opts = op.monitored_asset_opts;
         a.dynamic_asset_data_id = dyn_asset.id;
      });
   assert( new_asset.id == next_asset_id );

   return new_asset.id;
} FC_CAPTURE_AND_RETHROW( (op) ) }

void_result asset_issue_evaluator::do_evaluate( const asset_issue_operation& o )
{ try {
      const database& d = db();

      const asset_object& a = o.asset_to_issue.asset_id(d);
      FC_ASSERT( o.issuer == a.issuer );
      FC_ASSERT( !a.is_monitored_asset(), "Cannot manually issue a market-issued asset." );

      to_account = &o.issue_to_account(d);
      asset_dyn_data = &a.dynamic_asset_data_id(d);
      FC_ASSERT( (asset_dyn_data->current_supply + o.asset_to_issue.amount) <= a.options.max_supply );

      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }

void_result asset_issue_evaluator::do_apply( const asset_issue_operation& o )
{ try {
      db().adjust_balance( o.issue_to_account, o.asset_to_issue );

      db().modify( *asset_dyn_data, [&]( asset_dynamic_data_object& data ){
         data.current_supply += o.asset_to_issue.amount;
      });

      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }

void_result monitored_asset_update_evaluator::do_evaluate(const update_monitored_asset_operation& o)
{ try {
   database& d = db();

   const asset_object& a = o.asset_to_update(d);
   FC_ASSERT( a.is_monitored_asset() );

   asset_to_update = &a;
   FC_ASSERT( o.issuer == a.issuer, "", ("o.issuer", o.issuer)("a.issuer", a.issuer) );
   return void_result();
} FC_CAPTURE_AND_RETHROW((o)) }

void_result monitored_asset_update_evaluator::do_apply(const update_monitored_asset_operation& o)
{ try {
   database& d = db();

   d.modify(*asset_to_update, [&](asset_object& a) {
      if( o.new_description != "" )
         a.description = o.new_description;
      if(o.new_feed_lifetime_sec)
         a.monitored_asset_opts->feed_lifetime_sec = o.new_feed_lifetime_sec;
      if(o.new_minimum_feeds)
         a.monitored_asset_opts->minimum_feeds = o.new_minimum_feeds;
   });

   return void_result();
} FC_CAPTURE_AND_RETHROW( (o) ) }

void_result user_issued_asset_update_evaluator::do_evaluate(const update_user_issued_asset_operation& o)
{ try {
      database& d = db();

      const asset_object& a = o.asset_to_update(d);
      FC_ASSERT( !a.is_monitored_asset() && a.id != asset_id_type() );
      FC_ASSERT( a.options.max_supply >= a.dynamic_asset_data_id(d).current_supply );

      if( o.new_issuer )
         FC_ASSERT(d.find_object(*o.new_issuer));

      asset_to_update = &a;
      FC_ASSERT( o.issuer == a.issuer, "", ("o.issuer", o.issuer)("a.issuer", a.issuer) );

      return void_result();
   } FC_CAPTURE_AND_RETHROW((o)) }

void_result user_issued_asset_update_evaluator::do_apply(const update_user_issued_asset_operation& o)
{ try {
      database& d = db();

      d.modify(*asset_to_update, [&](asset_object& a) {
         if( o.new_issuer )
            a.issuer = *o.new_issuer;
         if( o.new_description != "" )
            a.description = o.new_description;
         a.options.max_supply = o.max_supply;
         a.options.core_exchange_rate = o.core_exchange_rate;
      });

      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }

void_result asset_reserve_evaluator::do_evaluate( const asset_reserve_operation& o )
{ try {
      const database& d = db();

      const asset_object& a = o.amount_to_reserve.asset_id(d);
      GRAPHENE_ASSERT(
         !a.is_monitored_asset(),
         asset_reserve_invalid_on_mia,
         "Cannot reserve ${sym} because it is a monitored asset",
         ("sym", a.symbol)
      );

      from_account = &o.payer(d);

      asset_dyn_data = &a.dynamic_asset_data_id(d);
      FC_ASSERT( (asset_dyn_data->current_supply - o.amount_to_reserve.amount) >= 0 );

      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }

void_result asset_reserve_evaluator::do_apply( const asset_reserve_operation& o )
{ try {
      db().adjust_balance( o.payer, -o.amount_to_reserve );

      db().modify( *asset_dyn_data, [&]( asset_dynamic_data_object& data ){
         data.current_supply -= o.amount_to_reserve.amount;
      });

      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }

void_result asset_fund_fee_pool_evaluator::do_evaluate(const asset_fund_fee_pool_operation& o)
{ try {
      database& d = db();

      const asset_object& a = o.asset_id(d);
      FC_ASSERT( !a.is_monitored_asset() );
      asset_dyn_data = &a.dynamic_asset_data_id(d);

      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }

void_result asset_fund_fee_pool_evaluator::do_apply(const asset_fund_fee_pool_operation& o)
{ try {
      db().adjust_balance(o.from_account, -o.amount);

      db().modify( *asset_dyn_data, [&]( asset_dynamic_data_object& data ) {
         data.fee_pool += o.amount;
      });

      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }

void_result asset_claim_fees_evaluator::do_evaluate( const asset_claim_fees_operation& o )
{ try {
      FC_ASSERT( o.amount_to_claim.asset_id(db()).issuer == o.issuer, "Asset fees may only be claimed by the issuer" );
      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }


void_result asset_claim_fees_evaluator::do_apply( const asset_claim_fees_operation& o )
{ try {
      database& d = db();

      const asset_object& a = o.amount_to_claim.asset_id(d);
      const asset_dynamic_data_object& addo = a.dynamic_asset_data_id(d);
      FC_ASSERT( o.amount_to_claim.amount <= addo.accumulated_fees, "Attempt to claim more fees than have accumulated", ("addo",addo) );

      d.modify( addo, [&]( asset_dynamic_data_object& _addo  ) {
         _addo.accumulated_fees -= o.amount_to_claim.amount;
      });

      d.adjust_balance( o.issuer, o.amount_to_claim );

      return void_result();
   } FC_CAPTURE_AND_RETHROW( (o) ) }

void_result asset_publish_feeds_evaluator::do_evaluate(const asset_publish_feed_operation& o)
{ try {
   database& d = db();

   const asset_object& asset_to_update = o.asset_id(d);
   //Verify that this feed is for a monitored asset and that asset is backed by the base
   FC_ASSERT( asset_to_update.is_monitored_asset());
   FC_ASSERT( d.get(GRAPHENE_MINER_ACCOUNT).active.account_auths.count(o.publisher) );
   FC_ASSERT( o.feed.core_exchange_rate.base.asset_id == asset_id_type() || o.feed.core_exchange_rate.quote.asset_id == asset_id_type() );
   FC_ASSERT( o.feed.core_exchange_rate.base.asset_id == o.asset_id || o.feed.core_exchange_rate.quote.asset_id == o.asset_id );

   return void_result();
} FC_CAPTURE_AND_RETHROW((o)) }

void_result asset_publish_feeds_evaluator::do_apply(const asset_publish_feed_operation& o)
{ try {

   database& d = db();

   const asset_object& asset_to_update = o.asset_id(d);
   auto old_feed =  asset_to_update.monitored_asset_opts->current_feed;
   // Store medians for this asset
   d.modify(asset_to_update , [&o,&d](asset_object& a) {
      a.monitored_asset_opts->feeds[o.publisher] = make_pair(d.head_block_time(), o.feed);
      a.monitored_asset_opts->update_median_feeds(d.head_block_time());
   });

   return void_result();
} FC_CAPTURE_AND_RETHROW((o)) }


} } // graphene::chain
