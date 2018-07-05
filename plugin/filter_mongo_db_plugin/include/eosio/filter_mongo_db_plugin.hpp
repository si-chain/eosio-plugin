//
// Created by hanfe on 2018/6/25.
//

#pragma once

#include <eosio/chain_plugin/chain_plugin.hpp>
#include <appbase/application.hpp>
#include <memory>
#include <boost/exception/diagnostic_information.hpp>

namespace bpo = boost::program_options;
using bpo::options_description;

namespace eosio {

    using filter_mongo_db_plugin_impl_ptr = std::shared_ptr<class filter_mongo_db_plugin_impl>;

/**
 * Provides persistence to MongoDB for:
 *   Blocks
 *   Transactions
 *   Actions
 *   Accounts
 *
 *   See data dictionary (DB Schema Definition - EOS API) for description of MongoDB schema.
 *
 *   The goal ultimately is for all chainbase data to be mirrored in MongoDB via a delayed node processing
 *   irreversible blocks. Currently, only Blocks, Transactions, Messages, and Account balance it mirrored.
 *   Chainbase is being rewritten to be multi-threaded. Once chainbase is stable, integration directly with
 *   a mirror database approach can be followed removing the need for the direct processing of Blocks employed
 *   with this implementation.
 *
 *   If MongoDB env not available (#ifndef MONGODB) this plugin is a no-op.
 */
    class filter_mongo_db_plugin : public appbase::plugin<filter_mongo_db_plugin> {
    public:
        APPBASE_PLUGIN_REQUIRES((chain_plugin))

        filter_mongo_db_plugin();
        virtual ~filter_mongo_db_plugin();

        virtual void set_program_options(options_description& cli, options_description& cfg) override;

        void plugin_initialize(const variables_map& options);
        void plugin_startup();
        void plugin_shutdown();

    private:
        filter_mongo_db_plugin_impl_ptr my;
    };

}
