# EOSIO PLUGIN - Extend EOSIO's plugin

In this project, is to extend the eosio's plugin.

## Use

Please replace the files in the same directory in the EOS project and add the plugins folder you want to use.

## List

* filter_mongo_db_plugin: code depend on the mongo_db_plugin;
  * function: depend on the config filter-contract to filter the contract's action which you want;
  * config: 
    * filter-mongodb-uri -- MongoDB URI connection string;
    * filter-mongodb-queue-size -- The target queue size between nodeos and MongoDB plugin thread;
    * filter-mongodb-wipe -- Required with --replay-blockchain, --hard-replay-blockchain, or --delete-all-blocks to wipe mongo db;
    * filter-contract -- Filter the contract actions by contract acccount name, use multiple;

