/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include <eosio/filter_mongo_db_plugin.hpp>
#include <eosio/chain/eosio_contract.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/types.hpp>

#include <fc/io/json.hpp>
#include <fc/variant.hpp>

#include <boost/chrono.hpp>
#include <boost/signals2/connection.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>

#include <queue>

#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>

namespace fc { class variant; }

namespace eosio {

using chain::account_name;
using chain::action_name;
using chain::block_id_type;
using chain::permission_name;
using chain::transaction;
using chain::signed_transaction;
using chain::signed_block;
using chain::block_trace;
using chain::transaction_id_type;
using chain::packed_transaction;

static appbase::abstract_plugin& _filter_mongo_db_plugin = app().register_plugin<filter_mongo_db_plugin>();

class filter_mongo_db_plugin_impl {
public:
   filter_mongo_db_plugin_impl();
   ~filter_mongo_db_plugin_impl();

   fc::optional<boost::signals2::scoped_connection> accepted_transaction_connection;

   void accepted_transaction(const chain::transaction_metadata_ptr&);
   void process_accepted_transaction(const chain::transaction_metadata_ptr&);
   void _process_accepted_transaction(const chain::transaction_metadata_ptr&);

   void init();
   void wipe_database();

   bool configured{false};
   bool wipe_database_on_startup{false};
   uint32_t start_block_num = 0;
   bool start_block_reached = false;

   vector<string>  filter_contract;

   std::string db_name;
   mongocxx::instance mongo_inst;
   mongocxx::client mongo_conn;
   mongocxx::collection accounts;

   size_t queue_size = 0;
   std::deque<chain::transaction_metadata_ptr> transaction_metadata_queue;
   std::deque<chain::transaction_metadata_ptr> transaction_metadata_process_queue;

   // transaction.id -> actions
   std::map<std::string, std::vector<chain::action>> reversible_actions;
   boost::mutex mtx;
   boost::condition_variable condition;
   boost::thread consume_thread;
   boost::atomic<bool> done{false};
   boost::atomic<bool> startup{true};
   fc::optional<chain::chain_id_type> chain_id;

   void consume_blocks();

   static const account_name newaccount;
   static const account_name setabi;

   static const std::string accounts_col;
   static const std::string filter_col;
};

const account_name filter_mongo_db_plugin_impl::newaccount = "newaccount";
const account_name filter_mongo_db_plugin_impl::setabi = "setabi";

const std::string filter_mongo_db_plugin_impl::filter_col = "filter";
const std::string filter_mongo_db_plugin_impl::accounts_col = "accounts";

namespace {

template<typename Queue, typename Entry>
void queue(boost::mutex& mtx, boost::condition_variable& condition, Queue& queue, const Entry& e, size_t queue_size) {
   int sleep_time = 100;
   size_t last_queue_size = 0;
   boost::mutex::scoped_lock lock(mtx);
   if (queue.size() > queue_size) {
      lock.unlock();
      condition.notify_one();
      if (last_queue_size < queue.size()) {
         sleep_time += 100;
      } else {
         sleep_time -= 100;
         if (sleep_time < 0) sleep_time = 100;
      }
      last_queue_size = queue.size();
      boost::this_thread::sleep_for(boost::chrono::milliseconds(sleep_time));
      lock.lock();
   }
   queue.emplace_back(e);
   lock.unlock();
   condition.notify_one();
}

}

void filter_mongo_db_plugin_impl::accepted_transaction( const chain::transaction_metadata_ptr& t ) {
   try {
      if( startup ) {
         // on startup we don't want to queue, instead push back on caller
         process_accepted_transaction( t );
      } else {
         queue( mtx, condition, transaction_metadata_queue, t, queue_size );
      }
   } catch (fc::exception& e) {
      elog("FC Exception while accepted_transaction ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while accepted_transaction ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while accepted_transaction");
   }
}


void filter_mongo_db_plugin_impl::consume_blocks() {
   try {
      while (true) {
         boost::mutex::scoped_lock lock(mtx);
         while ( transaction_metadata_queue.empty() &&
                 !done ) {
            condition.wait(lock);
         }

         // capture for processing
         size_t transaction_metadata_size = transaction_metadata_queue.size();
         if (transaction_metadata_size > 0) {
            transaction_metadata_process_queue = move(transaction_metadata_queue);
            transaction_metadata_queue.clear();
         }

         lock.unlock();

         // warn if queue size greater than 75%

         if( transaction_metadata_size > (queue_size * 0.75) ) {
            //wlog("queue size: ${q}", ("q", transaction_metadata_size + transaction_trace_size + block_state_size + irreversible_block_size));
            wlog("queue size: ${q}", ("q", transaction_metadata_size ));
         } else if (done) {
            ilog("draining queue, size: ${q}", ("q", transaction_metadata_size ));
         }

         // process transactions
         while (!transaction_metadata_process_queue.empty()) {
            const auto& t = transaction_metadata_process_queue.front();
            process_accepted_transaction(t);
            transaction_metadata_process_queue.pop_front();
         }

        if( transaction_metadata_size == 0 &&
             done ) {
            break;
         }
      }
      ilog("filter_mongo_db_plugin consume thread shutdown gracefully");
   } catch (fc::exception& e) {
      elog("FC Exception while consuming block ${e}", ("e", e.to_string()));
   } catch (std::exception& e) {
      elog("STD Exception while consuming block ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while consuming block");
   }
}

namespace {

   auto find_account(mongocxx::collection& accounts, const account_name& name) {
      using bsoncxx::builder::basic::make_document;
      using bsoncxx::builder::basic::kvp;
      return accounts.find_one( make_document( kvp( "name", name.to_string())));
   }

   auto find_transaction(mongocxx::collection& trans, const string& id) {
      using bsoncxx::builder::basic::make_document;
      using bsoncxx::builder::basic::kvp;
      return trans.find_one( make_document( kvp( "trx_id", id )));
   }


   optional<abi_serializer> get_abi_serializer( account_name n, mongocxx::collection& accounts ) {
      using bsoncxx::builder::basic::kvp;
      using bsoncxx::builder::basic::make_document;
      if( n.good()) {
         try {
            auto account = accounts.find_one( make_document( kvp("name", n.to_string())) );
            if(account) {
               auto view = account->view();
               abi_def abi;
               if( view.find( "abi" ) != view.end()) {
                  try {
                     abi = fc::json::from_string( bsoncxx::to_json( view["abi"].get_document())).as<abi_def>();
                  } catch (...) {
                     ilog( "Unable to convert account abi to abi_def for ${n}", ( "n", n ));
                     return optional<abi_serializer>();
                  }
                  return abi_serializer( abi );
               }
            }
         } FC_CAPTURE_AND_LOG((n))
      }
      return optional<abi_serializer>();
   }

   template<typename T>
   fc::variant to_variant_with_abi( const T& obj, mongocxx::collection& accounts ) {
      fc::variant pretty_output;
      abi_serializer::to_variant( obj, pretty_output, [&]( account_name n ) { return get_abi_serializer( n, accounts ); } );
      return pretty_output;
   }


   void update_account(mongocxx::collection& accounts, const chain::action& act) {
      using bsoncxx::builder::basic::kvp;
      using bsoncxx::builder::basic::make_document;
      using namespace bsoncxx::types;

      if (act.account != chain::config::system_account_name)
         return;

      try {
         if( act.name == filter_mongo_db_plugin_impl::newaccount ) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()} );
            auto newaccount = act.data_as<chain::newaccount>();

            // create new account
            if( !accounts.insert_one( make_document( kvp( "name", newaccount.name.to_string()),
                                                     kvp( "createdAt", b_date{now} )))) {
               elog( "Failed to insert account ${n}", ("n", newaccount.name));
            }

         } else if( act.name == filter_mongo_db_plugin_impl::setabi ) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()} );
            auto setabi = act.data_as<chain::setabi>();
            auto from_account = find_account( accounts, setabi.account );
            if( !from_account ) {
               if( !accounts.insert_one( make_document( kvp( "name", setabi.account.to_string()),
                                                        kvp( "createdAt", b_date{now} )))) {
                  elog( "Failed to insert account ${n}", ("n", setabi.account));
               }
               from_account = find_account( accounts, setabi.account );
            }
            if( from_account ) {
               try {
                  const abi_def& abi_def = fc::raw::unpack<chain::abi_def>( setabi.abi );
                  const string json_str = fc::json::to_string( abi_def );

                  auto update_from = make_document(
                        kvp( "$set", make_document( kvp( "abi", bsoncxx::from_json( json_str )),
                                                    kvp( "updatedAt", b_date{now} ))));

                  if( !accounts.update_one( make_document( kvp( "_id", from_account->view()["_id"].get_oid())), update_from.view()) ) {
                     elog( "Failed to udpdate account ${n}", ("n", setabi.account));
                  }
               } catch( fc::exception& e ) {
                  // if unable to unpack abi_def then just don't save the abi
                  // users are not required to use abi_def as their abi
               }
            }
         }
      } catch( fc::exception& e ) {
         // if unable to unpack native type, skip account creation
      }
   }

   void add_data(bsoncxx::builder::basic::document& act_doc, mongocxx::collection& accounts, const chain::action& act) {
      using bsoncxx::builder::basic::kvp;
      using bsoncxx::builder::basic::make_document;
      try {
         if( act.account == chain::config::system_account_name ) {
            if( act.name == filter_mongo_db_plugin_impl::newaccount ) {
               auto newaccount = act.data_as<chain::newaccount>();
               try {
                  auto json = fc::json::to_string( newaccount );
                  const auto& value = bsoncxx::from_json( json );
                  act_doc.append( kvp( "data", value ));
                  return;
               } catch (...) {
                  ilog( "Unable to convert action newaccount to json for ${n}", ( "n", newaccount.name.to_string() ));
               }
            } else if( act.name == filter_mongo_db_plugin_impl::setabi ) {
               auto setabi = act.data_as<chain::setabi>();
               try {
                  const abi_def& abi_def = fc::raw::unpack<chain::abi_def>( setabi.abi );
                  const string json_str = fc::json::to_string( abi_def );

                  // the original keys from document 'view' are kept, "data" here is not replaced by "data" of add_data
                  act_doc.append(
                        kvp( "data", make_document( kvp( "account", setabi.account.to_string()),
                                                    kvp( "abi_def", bsoncxx::from_json( json_str )))));
                  return;
               } catch( fc::exception& e ) {
                  ilog( "Unable to convert action abi_def to json for ${n}", ( "n", setabi.account.to_string() ));
               }
            }
         }
         auto account = find_account( accounts, act.account );
         if (account) {
            auto from_account = *account;
            abi_def abi;
            if( from_account.view().find( "abi" ) != from_account.view().end()) {
               try {
                  abi = fc::json::from_string( bsoncxx::to_json( from_account.view()["abi"].get_document())).as<abi_def>();
               } catch( ... ) {
                  ilog( "Unable to convert account abi to abi_def for ${s}::${n}", ("s", act.account)( "n", act.name ));
               }
            }
            string json;
            try {
               abi_serializer abis;
               abis.set_abi( abi );
               auto v = abis.binary_to_variant( abis.get_action_type( act.name ), act.data );
               json = fc::json::to_string( v );

               const auto& value = bsoncxx::from_json( json );
               act_doc.append( kvp( "data", value ));
               return;
            } catch( std::exception& e ) {
               elog( "Unable to convert EOS JSON to MongoDB JSON: ${e}", ("e", e.what()));
               elog( "  EOS JSON: ${j}", ("j", json));
            }
         }
      } catch (fc::exception& e) {
         if( act.name != "onblock" ) { // onblock not in original eosio.system contract abi
            dlog( "Unable to convert action.data to ABI: ${s}::${n}, what: ${e}",
                  ("s", act.account)( "n", act.name )( "e", e.to_detail_string()));
         }
      } catch (std::exception& e) {
         ilog( "Unable to convert action.data to ABI: ${s}::${n}, std what: ${e}",
               ("s", act.account)( "n", act.name )( "e", e.what()));
      } catch (...) {
         ilog( "Unable to convert action.data to ABI: ${s}::${n}, unknown exception",
               ("s", act.account)( "n", act.name ));
      }
      // if anything went wrong just store raw hex_data
      act_doc.append( kvp( "hex_data", fc::variant( act.data ).as_string()));
   }

}

void filter_mongo_db_plugin_impl::process_accepted_transaction( const chain::transaction_metadata_ptr& t ) {
   try {
      // always call since we need to capture setabi on accounts even if not storing transactions
      _process_accepted_transaction(t);
   } catch (fc::exception& e) {
      elog("FC Exception while processing accepted transaction metadata: ${e}", ("e", e.to_detail_string()));
   } catch (std::exception& e) {
      elog("STD Exception while processing accepted tranasction metadata: ${e}", ("e", e.what()));
   } catch (...) {
      elog("Unknown exception while processing accepted transaction metadata");
   }
}


void filter_mongo_db_plugin_impl::_process_accepted_transaction( const chain::transaction_metadata_ptr& t ) {
   using namespace bsoncxx::types;
   using bsoncxx::builder::basic::kvp;
   using bsoncxx::builder::basic::make_document;
   using bsoncxx::builder::basic::make_array;
   namespace bbb = bsoncxx::builder::basic;


   accounts = mongo_conn[db_name][accounts_col];
   auto filter = mongo_conn[db_name][filter_col];
   auto trans_doc = bsoncxx::builder::basic::document{};

   auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()});

   const auto trx_id = t->id;
   const auto trx_id_str = trx_id.str();
   const auto& trx = t->trx;
   const chain::transaction_header& trx_header = trx;

   bool actions_to_write = false;
   bool filter_to_write = false;
   mongocxx::options::bulk_write bulk_opts;
   bulk_opts.ordered(false);

   mongocxx::bulk_write bulk_filter = filter.create_bulk_write(bulk_opts);

   int32_t act_num = 0;
   auto process_action = [&](const std::string& trx_id_str, const chain::action& act, bbb::array& act_array, bool cfa) -> auto {
      auto act_doc = bsoncxx::builder::basic::document();
      if( start_block_reached ) {
         act_doc.append( kvp( "action_num", b_int32{act_num} ),
                         kvp( "trx_id", trx_id_str ));
         act_doc.append( kvp( "cfa", b_bool{cfa} ));
         act_doc.append( kvp( "account", act.account.to_string()));
         act_doc.append( kvp( "name", act.name.to_string()));
         act_doc.append( kvp( "authorization", [&act]( bsoncxx::builder::basic::sub_array subarr ) {
            for( const auto& auth : act.authorization ) {
               subarr.append( [&auth]( bsoncxx::builder::basic::sub_document subdoc ) {
                  subdoc.append( kvp( "actor", auth.actor.to_string()),
                                 kvp( "permission", auth.permission.to_string()));
               } );
            }
         } ));
      }
      try {
         update_account( accounts, act );
      } catch (...) {
         ilog( "Unable to update account for ${s}::${n}", ("s", act.account)( "n", act.name ));
      }
      if( start_block_reached ) {
         add_data( act_doc, accounts, act );
         act_array.append( act_doc );
         mongocxx::model::insert_one insert_op{act_doc.view()};

         auto it = std::find(filter_contract.begin(), filter_contract.end(), act.account.to_string());

         if ( it != filter_contract.end() ) {
            bulk_filter.append( insert_op );
            filter_to_write = true;
         }

         actions_to_write = true;
      }

      ++act_num;
      return act_num;
   };

   if( !trx.actions.empty()) {
      bsoncxx::builder::basic::array action_array;
      for( const auto& act : trx.actions ) {
         process_action( trx_id_str, act, action_array, false );
      }
      trans_doc.append( kvp( "actions", action_array ));
   }


   if( start_block_reached ) {

      if( actions_to_write ) {
        //  auto result = bulk_actions.execute();
        //  if( !result ) {
        //     elog( "Bulk actions insert failed for transaction: ${id}", ("id", trx_id_str));
        //  }
         if ( filter_to_write ) {
            auto result = bulk_filter.execute();
            if( !result ) {
                elog( "Bulk sic insert failed for transaction: ${id}", ("id", trx_id_str));
            }
         }
         
      }
   }
}


filter_mongo_db_plugin_impl::filter_mongo_db_plugin_impl()
: mongo_inst{}
, mongo_conn{}
{
}

filter_mongo_db_plugin_impl::~filter_mongo_db_plugin_impl() {
   if (!startup) {
      try {
         ilog( "filter_mongo_db_plugin shutdown in process please be patient this can take a few minutes" );
         done = true;
         condition.notify_one();

         consume_thread.join();
      } catch( std::exception& e ) {
         elog( "Exception on filter_mongo_db_plugin shutdown of consume thread: ${e}", ("e", e.what()));
      }
   }
}

void filter_mongo_db_plugin_impl::wipe_database() {
   ilog("mongo db wipe_database");

   auto contract = mongo_conn[db_name][filter_col];
   accounts = mongo_conn[db_name][accounts_col];

   contract.drop();
   accounts.drop();
}

void filter_mongo_db_plugin_impl::init() {
   using namespace bsoncxx::types;
   using bsoncxx::builder::basic::make_document;
   using bsoncxx::builder::basic::kvp;
   // Create the native contract accounts manually; sadly, we can't run their contracts to make them create themselves
   // See native_contract_chain_initializer::prepare_database()

   accounts = mongo_conn[db_name][accounts_col];
   if (accounts.count(make_document()) == 0) {
      auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::microseconds{fc::time_point::now().time_since_epoch().count()});

      auto doc = make_document( kvp( "name", name( chain::config::system_account_name ).to_string()),
                                kvp( "createdAt", b_date{now} ));

      if (!accounts.insert_one(doc.view())) {
         elog("Failed to insert account ${n}", ("n", name(chain::config::system_account_name).to_string()));
      }

   }
}

////////////
// filter_mongo_db_plugin
////////////

filter_mongo_db_plugin::filter_mongo_db_plugin()
:my(new filter_mongo_db_plugin_impl)
{
}

filter_mongo_db_plugin::~filter_mongo_db_plugin()
{
}

void filter_mongo_db_plugin::set_program_options(options_description& cli, options_description& cfg)
{
   cfg.add_options()
         ("filter-contract", bpo::value< vector<string> >()->composing(), "Filter the contract actions by contract acccount name.") 
         ("filter-mongodb-queue-size,q", bpo::value<uint32_t>()->default_value(256),
         "The target queue size between nodeos and MongoDB plugin thread.")
         ("filter-mongodb-wipe", bpo::bool_switch()->default_value(false),
         "Required with --replay-blockchain, --hard-replay-blockchain, or --delete-all-blocks to wipe mongo db."
         "This option required to prevent accidental wipe of mongo db.")
         ("filter-mongodb-block-start", bpo::value<uint32_t>()->default_value(0),
         "If specified then no data pushed to mongodb until accepted block is reached.")
         ("filter-mongodb-uri,m", bpo::value<std::string>(),
         "MongoDB URI connection string, see: https://docs.mongodb.com/master/reference/connection-string/."
               " If not specified then plugin is disabled. Default database 'EOS' is used if not specified in URI."
               " Example: mongodb://127.0.0.1:27017/EOS")         
         ;
}

void filter_mongo_db_plugin::plugin_initialize(const variables_map& options)
{
   try {
      if( options.count( "filter-mongodb-uri" )) {
         ilog( "initializing filter_mongo_db_plugin" );
         my->configured = true;

         if( options.at( "replay-blockchain" ).as<bool>() || options.at( "hard-replay-blockchain" ).as<bool>() || options.at( "delete-all-blocks" ).as<bool>() ) {
            if( options.at( "filter-mongodb-wipe" ).as<bool>()) {
               ilog( "Wiping mongo database on startup" );
               my->wipe_database_on_startup = true;
            } else {
               FC_ASSERT( false, "--filter-mongodb-wipe required with --replay-blockchain, --hard-replay-blockchain, or --delete-all-blocks"
                                 " --filter-mongodb-wipe will remove all EOS collections from mongodb." );
            }
         }

         if( options.count( "filter-mongodb-queue-size" )) {
            my->queue_size = options.at( "filter-mongodb-queue-size" ).as<uint32_t>();
         }
         if( options.count( "filter-mongodb-block-start" )) {
            my->start_block_num = options.at( "filter-mongodb-block-start" ).as<uint32_t>();
         }
         if( my->start_block_num == 0 ) {
            my->start_block_reached = true;
         }
         
         if( options.count("filter-contract") ) {
            my->filter_contract = options.at("filter-contract").as<vector<string> >();
         }
         for( auto contractinfo : my->filter_contract ) {
            ilog( "filter contract: ${c}", ("c", contractinfo) );
         }

         std::string uri_str = options.at( "filter-mongodb-uri" ).as<std::string>();
         ilog( "connecting to ${u}", ("u", uri_str));
         mongocxx::uri uri = mongocxx::uri{uri_str};
         my->db_name = uri.database();
         if( my->db_name.empty())
            my->db_name = "Filter";
         my->mongo_conn = mongocxx::client{uri};

         // hook up to signals on controller
         chain_plugin* chain_plug = app().find_plugin<chain_plugin>();
         FC_ASSERT( chain_plug );
         auto& chain = chain_plug->chain();
         my->chain_id.emplace( chain.get_chain_id());

         my->accepted_transaction_connection.emplace(
               chain.accepted_transaction.connect( [&]( const chain::transaction_metadata_ptr& t ) {
                  my->accepted_transaction( t );
               } ));

         if( my->wipe_database_on_startup ) {
            my->wipe_database();
         }
         my->init();
      } else {
         wlog( "eosio::filter_mongo_db_plugin configured, but no --mongodb-uri specified." );
         wlog( "filter_mongo_db_plugin disabled." );
      }
   } FC_LOG_AND_RETHROW()
}

void filter_mongo_db_plugin::plugin_startup()
{
   if (my->configured) {
      ilog("starting db plugin");

      my->consume_thread = boost::thread([this] { my->consume_blocks(); });

      my->startup = false;
   }
}

void filter_mongo_db_plugin::plugin_shutdown()
{
   my->accepted_transaction_connection.reset();

   my.reset();
}

} // namespace eosio
