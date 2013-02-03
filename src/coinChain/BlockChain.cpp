/* -*-c++-*- libcoin - Copyright (C) 2012 Michael Gronager
 *
 * libcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * libcoin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libcoin.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <coinChain/BlockChain.h>

#include <coin/Block.h>
#include <coinChain/MessageHeader.h>

#include <coinChain/Peer.h>

#include <coin/Script.h>
#include <coin/Logger.h>

#include <boost/lexical_cast.hpp>

#include <numeric>

using namespace std;
using namespace boost;
using namespace sqliterate;


//
// BlockChain
//

BlockChain::BlockChain(const Chain& chain, const string dataDir) :
    sqliterate::Database(dataDir == "" ? ":memory:" : dataDir + "/blockchain.sqlite3"),
    _chain(chain),
    _verifier(0),
    _lazy_purging(false),
    _purge_depth(0), // means no purging - i.e.
    _verification_depth(_chain.totalBlocksEstimate())
{
    _acceptBlockTimer = 0;
    _connectInputsTimer = 0;
    _verifySignatureTimer = 0;
    _setBestChainTimer = 0;
    _addToBlockIndexTimer = 0;
    _bestReceivedTime = 0;
    
    // setup the database tables
    // The blocks points backwards, so they could create a tree. Which tree to choose ? The best of them... So each time a new block is inserted, it is checked against the main chain. If the main chain needs to be updated it will be.
    
    query("PRAGMA journal_mode=WAL");
    query("PRAGMA locking_mode=NORMAL");
    query("PRAGMA synchronous=OFF");
    query("PRAGMA page_size=16384"); // this is 512MiB of cache with 4kiB page_size
    query("PRAGMA cache_size=131072"); // this is 512MiB of cache with 4kiB page_size
    query("PRAGMA temp_store=MEMORY"); // use memory for temp tables
    
    query("CREATE TABLE IF NOT EXISTS Blocks ("
              "count INTEGER PRIMARY KEY," // block count is height+1 - i.e. the genesis has count = 1
              "hash BINARY,"
              "version INTEGER,"
              "prev BINARY,"
              "mrkl BINARY,"
              "time INTEGER,"
              "bits INTEGER,"
              "nonce INTEGER"
          ")");
    
    query("CREATE TABLE IF NOT EXISTS Confirmations ("
              "cnf INTEGER PRIMARY KEY AUTOINCREMENT," // coinbase transactions have cnf = -count
              "version INTEGER,"
              "locktime INTEGER,"
              "count INTEGER," // this points to the block where the Transaction was confirmed
              "idx INTEGER"
          ")");

    query("CREATE TABLE IF NOT EXISTS Unspents ("
              "coin INTEGER PRIMARY KEY AUTOINCREMENT,"
              "hash BINARY,"
              "idx INTEGER,"
              "value INTEGER,"
              "script BINARY,"
              "count INTEGER," // count is the block count where the spendable is confirmed (this can also be found in the confirmation, but we need it here for fast access) -- coinbases have negative height - the group of spendables are those unspent with a count bigger than -(MAX(count)-100)
              "ocnf INTEGER REFERENCES Confirmations(cnf)" // if this is < 0 the coin is part of a coinbase tx
          ")");
    
    query("CREATE INDEX IF NOT EXISTS UnspentsOut ON Unspents (ocnf)");

    query("CREATE INDEX IF NOT EXISTS UnspentCount ON Unspents(count)");
    
    query("CREATE TABLE IF NOT EXISTS Spendings ("
              "ocnf INTEGER REFERENCES Confirmations(cnf)," // ocnf is the confirmation that introduced the coin
              "coin INTEGER PRIMARY KEY," // this follows the same counting as the Unspents, except for coinbases, which has coin = -count
              "hash BINARY,"
              "idx INTEGER,"
              "value INTEGER,"
              "script BINARY,"
              "signature BINARY,"
              "sequence INTEGER,"
              "icnf INTEGER REFERENCES Confirmations(cnf)" // icnf is the confirmation that spent the coin
          ")");

    //    query("CREATE INDEX IF NOT EXISTS SpendingIndex ON Spendings (hash, idx)");

    query("CREATE INDEX IF NOT EXISTS SpendingsIn ON Spendings (icnf)");
    query("CREATE INDEX IF NOT EXISTS SpendingsOut ON Spendings (ocnf)");
    
    // populate the tree
    vector<BlockRef> blockchain = queryColRow<BlockRef(int, uint256, uint256, unsigned int, unsigned int)>("SELECT version, hash, prev, time, bits FROM Blocks ORDER BY count");
    _tree.assign(blockchain);
        
    if (_tree.count() == 0) { // there are no blocks, insert the genesis block
        Block block = _chain.genesisBlock();
        blockchain.push_back(BlockRef(block.getVersion(), block.getHash(), block.getPrevBlock(), block.getBlockTime(), block.getBits()));
        _tree.assign(blockchain);
        _branches[block.getHash()] = block;
        try {
            query("BEGIN --GENESIS");
            Txns txns;
            Hashes hashes;
            BlockIterator blk = _tree.find(block.getHash());
            attach(blk, txns, hashes);
            query("COMMIT --GENESIS");
        }
        catch (std::exception& e) {
            query("ROLLBACK --GENESIS");
            throw Error(string("BlockChain - creating genesisblock failed: ") + e.what());
        }
        catch (...) {
            query("ROLLBACK --GENESIS");
            throw Error("BlockChain - creating genesisblock failed");
        }
        _branches.clear();
    }
    updateBestLocator();
    log_info("BlockChain initialized - main best height: %d", _tree.height());
    
    // determine purge_depth from database:
    _purge_depth = query<int64>("SELECT CASE WHEN COUNT(*)=0 THEN 0 ELSE MIN(count) END FROM Confirmations");
    
    // determine validation index type from database:
    bool coin_index = query<int64>("SELECT COUNT(*) FROM SQLITE_MASTER WHERE name='UnspentIndex'");
    if (coin_index)
        _validation_depth = 0;
    else {
        _validation_depth = _chain.totalBlocksEstimate();

        // load the elements - i.e. the spendables
        Unspents spendables = queryColRow<Unspent(int64, uint256, unsigned int, int64, Script, int64, int64)>("SELECT coin, hash, idx, value, script, count, ocnf FROM Unspents WHERE count >= -?", _tree.count()-COINBASE_MATURITY);
        
        for (Unspents::const_iterator u = spendables.begin(); u != spendables.end(); ++u)
            _spendables.insert(*u);
        
        Unspents immatures = queryColRow<Unspent(int64, uint256, unsigned int, int64, Script, int64, int64)>("SELECT coin, hash, idx, value, script, count, ocnf FROM Unspents WHERE count < -?", _tree.count()-COINBASE_MATURITY);

        for (Unspents::const_iterator u = immatures.begin(); u != immatures.end(); ++u)
            _immature_coinbases.insert(*u);
    }    
}

unsigned int BlockChain::purge_depth() const {
    return _purge_depth;
}

void BlockChain::purge_depth(unsigned int purge_depth) {
    _purge_depth = purge_depth;
    query("DELETE FROM Spendings WHERE icnf IN (SELECT cnf FROM Confirmations WHERE count <= ?)", _purge_depth);
    query("DELETE FROM Confirmations WHERE count <= ?", _purge_depth);
}

void BlockChain::validation_depth(unsigned int v) {
    if (v == _validation_depth) return;

    _validation_depth = v;
    
    if (_validation_depth == 0) {
        query("CREATE UNIQUE INDEX IF NOT EXISTS UnspentIndex ON Unspents (hash, idx)");
    }
    else {
        query("DROP INDEX IF EXISTS UnspentIndex");
        if (_tree.count() < _validation_depth)
            _spendables.authenticated(false);
        else {
            _spendables.authenticated(true);
            log_info("MerkleTrie Hashing on with root hash: %s", _spendables.root()->hash().toString());
        }
    }
}

bool BlockChain::script_to_unspents() const {
    return query<int64>("SELECT COUNT(*) FROM SQLITE_MASTER WHERE name='ScriptIndex'");
}

void BlockChain::script_to_unspents(bool enable) {
    if (enable)
        query("CREATE INDEX IF NOT EXISTS ScriptIndex ON Unspents (script)");
    else
        query("DROP INDEX IF EXISTS ScriptIndex");
}

std::pair<Claims::Spents, int64> BlockChain::try_claim(const Transaction& txn, bool verify) const {
    uint256 hash = txn.getHash();
    int64 fee = 0;
    int64 min_fee = 0;
    
    if (_claims.have(hash)) // transaction already exist
        throw Error("Transaction already exists!");
    
    Claims::Spents spents;
    
    try {
        
        // BIP0016 check - if the time is newer than the BIP0016 date enforce strictPayToScriptHash
        bool strictPayToScriptHash = (GetTime() > _chain.timeStamp(Chain::BIP0016));
        
        // redeem the inputs
        const Inputs& inputs = txn.getInputs();
        int64 value_in = 0;
        for (size_t in_idx = 0; in_idx < inputs.size(); ++in_idx) {
            Unspent coin;
            const Input& input = inputs[in_idx];
            //  Already marked as spent? - either in an earlier claim or in this claim
            if (_claims.spent(input.prevout()) || spents.count(input.prevout()))
                throw Error("Coin already spent!");
            
            //  Among the outputs of a former, active, claim? - lookup the out hash in _unconfirmed / _claims
            Output output = _claims.prev(input.prevout());
            if (!output.isNull()) {
                coin = Unspent(0, input.prevout().hash, input.prevout().index, output.value(), output.script(), 0, 0);
            }
            else {
                //  3. are among the confirmed outputs in the Database / do a database lookup
                if (_validation_depth == 0) {
                    coin = queryRow<Unspent(int64, uint256, unsigned int, int64, Script, int64, int64)>("SELECT coin, hash, idx, value, script, count, ocnf FROM Unspents WHERE hash = ? AND idx = ?", input.prevout().hash, input.prevout().index);
                    if (!coin)
                        throw Reject("Spent coin not found !");
                    
                    if (coin.count < 0 && _tree.count() + coin.count < COINBASE_MATURITY)
                        throw Error("Tried to spend immature coinbase");
                }
                else {
                    Spendables::Iterator is = _spendables.find(input.prevout());
                    if (!!is)
                        coin = *is;
                    else
                        throw Reject("Spent coin not found or immature coinbase");
                }
            }
            spents.insert(input.prevout());
            // all OK - spend the coin
            
            // Check for negative or overflow input values
            if (!MoneyRange(coin.output.value()))
                throw Error("Input values out of range");
            
            value_in += coin.output.value();
            
            if (verify) // this is invocation only - the actual verification takes place in other threads
                if(!VerifySignature(coin.output, txn, in_idx, strictPayToScriptHash, 0))
                    throw Error("Verify Signature failed with verifying: " + txn.getHash().toString());
        }
        
        // verify outputs
        fee = value_in - txn.getValueOut();
        if (fee < 0)
            throw Error("fee < 0");
        if (fee < min_fee)
            throw Error("fee < min_fee");
        
    }
    catch (Reject& r) {
        throw Reject(string("claim(Transaction): ") + r.what());
    }
    catch (std::exception& e) {
        throw Error(string("claim(Transaction): ") + e.what());
    }

    return make_pair<Claims::Spents, int64>(spents, fee);
}

// claim, claims a transaction expecting it to go into a block in the near future
void BlockChain::claim(const Transaction& txn, bool verify) {
    pair<Claims::Spents, int64> res = try_claim(txn, verify);
    
    // we insert the unconfirmed transaction into a list/map according to the key: fee/size and delta-spendings    
    _claims.insert(txn, res.first, res.second);
}

Output BlockChain::redeem(const Input& input, Confirmation iconf) {
    Unspent coin;
    
    if (_validation_depth == 0) {
        coin = queryRow<Unspent(int64, uint256, unsigned int, int64, Script, int64, int64)>("SELECT coin, hash, idx, value, script, count, ocnf FROM Unspents WHERE hash = ? AND idx = ?", input.prevout().hash, input.prevout().index);

        if (!coin)
            throw Reject("Spent coin not found !");

        if (coin.count < 0 && iconf.count + coin.count < COINBASE_MATURITY)
            throw Error("Tried to spend immature coinbase");
    }
    else {
        _redeemStats.start();
        Spendables::Iterator is;
        is = _spendables.find(input.prevout());
        
        if (!!is)
            coin = *is;
        else
            throw Error("Spent coin not found or immature coinbase");
        
        _spendables.remove(is);
        _redeemStats.stop();
    }
    
    // all OK - spend the coin
    
    // Check for negative or overflow input values
    if (!MoneyRange(coin.output.value()))
        throw Error("Input values out of range");

    if (iconf.count >= _purge_depth)
        query("INSERT INTO Spendings (coin, ocnf, hash, idx, value, script, signature, sequence, icnf) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", coin.coin, coin.cnf, input.prevout().hash, input.prevout().index, coin.output.value(), coin.output.script(), input.signature(), input.sequence(), iconf.cnf);
    query("DELETE FROM Unspents WHERE coin = ?", coin.coin);
    
    return coin.output;
}

void BlockChain::issue(const Output& output, uint256 hash, unsigned int out_idx, Confirmation conf, bool unique) {
    int64 count = conf.is_coinbase() ? -conf.count : conf.count;
    if (_validation_depth == 0) {
        if (unique)
            query("INSERT INTO Unspents (hash, idx, value, script, count, ocnf) VALUES (?, ?, ?, ?, ?, ?)", hash, out_idx, output.value(), output.script(), count, conf.cnf); // will throw if trying to insert a dublicate value as the index is unique
        else
            query("INSERT OR REPLACE INTO Unspents (hash, idx, value, script, count, ocnf) VALUES (?, ?, ?, ?, ?, ?)", hash, out_idx, output.value(), output.script(), count, conf.cnf);
    }
    else {
        int64 coin = query("INSERT INTO Unspents (hash, idx, value, script, count, ocnf) VALUES (?, ?, ?, ?, ?, ?)", hash, out_idx, output.value(), output.script(), count, conf.cnf);
        
        _issueStats.start();
        Unspent unspent(coin, hash, out_idx, output.value(), output.script(), count, conf.cnf);

        // we need to test uniqueness explicitly of coinbases among the other immature coinbases
        if (conf.is_coinbase()) {
            if (unique) {
                Spendables::Iterator cb = _immature_coinbases.find(Coin(hash, out_idx));
                if (!!cb)
                    throw Error("Attempting to insert dublicate coinbase");
                cb = _spendables.find(Coin(hash, out_idx));
                if (!!cb)
                    throw Error("Attempting to insert dublicate coinbase");
            }
            _immature_coinbases.insert(unspent);
        }
        else
            _spendables.insert(unspent);
        _issueStats.stop();
    }
}

void BlockChain::maturate(int64 count) {
    if (_validation_depth == 0)
        return;
    
    Unspents coinbase_unspents = queryColRow<Unspent(int64, uint256, unsigned int, int64, Script, int64, int64)>("SELECT coin, hash, idx, value, script, count, ocnf FROM Unspents WHERE count = ?", -count);

    for (Unspents::const_iterator cb = coinbase_unspents.begin(); cb != coinbase_unspents.end(); ++cb) {
        _spendables.insert(*cb);
        _immature_coinbases.remove(cb->key);
    }
}

void BlockChain::insertBlockHeader(int64 count, const Block& block) {
    query("INSERT INTO Blocks (count, hash, version, prev, mrkl, time, bits, nonce) VALUES (?, ?, ?, ?, ?, ?, ?, ?)", count, block.getHash(), block.getVersion(), block.getPrevBlock(), block.getMerkleRoot(), block.getBlockTime(), block.getBits(), block.getNonce());
}

void BlockChain::postTransaction(const Transaction txn, int64& fees, int64 min_fee, BlockIterator blk, int64 idx, bool verify) {
    Confirmation conf(txn, 0, blk.count());
    
    uint256 hash = txn.getHash();
    
    // BIP0016 check - if the block is newer than the BIP0016 date enforce strictPayToScriptHash
    bool strictPayToScriptHash = (blk->time > _chain.timeStamp(Chain::BIP0016));
    
    if (blk.count() >= _purge_depth)
        conf.cnf = query("INSERT INTO Confirmations (locktime, version, count, idx) VALUES (?, ?, ?, ?)", txn.lockTime(), txn.version(), blk.count(), idx);
    else
        conf.cnf = LOCKTIME_THRESHOLD; // we are downloading the chain - no need to create a confirmation
        
    // redeem the inputs
    const Inputs& inputs = txn.getInputs();
    int64 value_in = 0;
    for (size_t in_idx = 0; in_idx < inputs.size(); ++in_idx) {
        const Input& input = inputs[in_idx];
        Output coin = redeem(input, conf); // this will throw in case of doublespend attempts
        value_in += coin.value();
        
        _verifySignatureTimer -= GetTimeMicros();
        
        if (verify) // this is invocation only - the actual verification takes place in other threads
            _verifier.verify(coin, txn, in_idx, strictPayToScriptHash, 0);
        
        _verifySignatureTimer += GetTimeMicros();
    }
    
    // verify outputs
    int64 fee = value_in - txn.getValueOut();
    if (fee < 0)
        throw Error("fee < 0");
    if (fee < min_fee)
        throw Error("fee < min_fee");
    fees += fee;
    if (!MoneyRange(fees))
        throw Error("fees out of range");
    
    // issue the outputs
    const Outputs& outputs = txn.getOutputs();
    for (size_t out_idx = 0; out_idx < outputs.size(); ++out_idx)
        issue(outputs[out_idx], hash, out_idx, conf); // will throw in case of dublicate (hash,idx)
}

void BlockChain::postSubsidy(const Transaction txn, BlockIterator blk, int64 fees) {
    
    if (!txn.isCoinBase())
        throw Error("postSubsidy only valid for coinbase transactions.");
    
    Confirmation conf(txn, 0, blk.count());
    
    uint256 hash = txn.getHash();
    
    if (blk.count() >= _purge_depth)
        conf.cnf = query("INSERT INTO Confirmations (cnf, locktime, version, count, idx) VALUES (?, ?, ?, ?, ?)", -blk.count(), txn.lockTime(), txn.version(), blk.count(), 0);
    else
        conf.cnf = -blk.count();

    // create the transaction and check that it is not spending already spent coins
    
    // insert coinbase into spendings
    const Input& input = txn.getInput(0);
    int64 value_in = _chain.subsidy(blk.height()) + fees;
    if (value_in < txn.getValueOut())
        throw Error("value in < value out");
    if (blk.count() >= _purge_depth)
        query("INSERT INTO Spendings (ocnf, coin, hash, idx, value, script, signature, sequence, icnf) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", 0, -blk.count(), uint256(0), 0, value_in, Script(), input.signature(), input.sequence(), blk.count());
    
    // issue the outputs
    
    // BIP0030 check - transactions must be unique after a certain timestamp
    bool unique = blk->time > _chain.timeStamp(Chain::BIP0030);
    
    const Outputs& outputs = txn.getOutputs();
    for (size_t out_idx = 0; out_idx < outputs.size(); ++out_idx)
        issue(outputs[out_idx], hash, out_idx, conf, unique);

    if (_validation_depth > 0 && blk.count() > COINBASE_MATURITY)
        maturate(blk.count()-COINBASE_MATURITY);
}

void BlockChain::rollbackConfirmation(int64 cnf) {
    // first get a list of spendings in which this coin was used as input and delete these iteratively...
    //    vector<int64> cnfs = queryCol<int64>("SELECT ocnf FROM Spendings WHERE icnf = ?");
    //for (size_t i = 0; i < cnfs.size(); ++i)
    //  rollbackConfirmation(cnfs[i], true);
    // delete the coins

    int64 count = query<int64, int64>("SELECT count FROM Confirmations WHERE cnf = ?", cnf);
    
    if (_validation_depth > 0) {
        // iterate over spendings and undo them by converting spengins to unspents and remove correspoding unspent
        if (cnf > 0) {
            Unspents unspents = queryColRow<Unspent(int64, uint256, unsigned int, int64, Script, int64, int64)>("SELECT coin, hash, idx, value, script, ?, ocnf FROM Spendings WHERE icnf = ?", count, cnf); // we lose the block info count here !
            
            for (Unspents::const_iterator u = unspents.begin(); u != unspents.end(); ++u)
                _spendables.insert(*u);
            
            vector<Coin> coins = queryColRow<Coin(uint256, unsigned int)>("SELECT hash, idx FROM Unspents WHERE ocnf = ?", cnf);
            for (vector<Coin>::const_iterator c = coins.begin(); c != coins.end(); ++c)
                _spendables.remove(*c);
        }
    }
    
    if (cnf < 0) count = -count;
    
    query("INSERT INTO Unspents (coin, hash, idx, value, script, count, ocnf) SELECT coin, hash, idx, value, script, ?, ocnf FROM Spendings WHERE icnf = ?", count, cnf);
    query("DELETE FROM Spendings WHERE icnf = ?", cnf);
    
    query("DELETE FROM Unspents WHERE ocnf = ?", cnf);

    query("DELETE FROM Confirmations WHERE cnf = ?", cnf);
}

void BlockChain::rollbackBlock(int count) {
    typedef vector<int64> Cnfs;
    Cnfs cnfs = queryCol<int64>("SELECT cnf FROM Confirmations WHERE count = ? ORDER BY idx", count);
    // remove transactions in reverse order
    for (Cnfs::const_reverse_iterator tx = cnfs.rbegin(); tx != cnfs.rend(); ++tx) {
        rollbackConfirmation(*tx);
    }
    query("DELETE FROM Blocks WHERE count = ?", count);
}

void BlockChain::getBlockHeader(int count, Block& block) const {
    block = queryRow<Block(int, uint256, uint256, int, int, int)>("SELECT version, prev, mrkl, time, bits, nonce FROM Blocks WHERE count = ?", count);
}

void BlockChain::getBlock(int count, Block& block) const {
    block.setNull();

    getBlockHeader(count, block);
    
    // now get the transactions
    vector<Confirmation> confs = queryColRow<Confirmation(int, unsigned int, int64, int64)>("SELECT version, locktime, cnf, count FROM Confirmations WHERE count = ? ORDER BY idx", count);
    
    for (size_t idx = 0; idx < confs.size(); idx++) {
        Inputs inputs = queryColRow<Input(uint256, unsigned int, Script, unsigned int)>("SELECT hash, idx, signature, sequence FROM Spendings WHERE icnf = ? ORDER BY idx", confs[idx].cnf);
        Outputs outputs = queryColRow<Output(int64, Script)>("SELECT value, script FROM (SELECT value, script, idx FROM Unspents WHERE ocnf = ?1 UNION SELECT value, script, idx FROM Spendings WHERE icnf = ?1 ORDER BY idx ASC);", confs[idx].cnf);
        Transaction txn = confs[idx];
        txn.setInputs(inputs);
        txn.setOutputs(outputs);
        block.addTransaction(txn);
    }
}

void BlockChain::attach(BlockIterator &blk, Txns& unconfirmed, Hashes& confirmed) {
    Block block = _branches[blk->hash];
    int height = blk.height(); // height for non trunk blocks is negative
    
    if (!_chain.checkPoints(height, blk->hash))
        throw Error("Rejected by checkpoint lockin at " + lexical_cast<string>(height));
    
    for(size_t idx = 0; idx < block.getNumTransactions(); ++idx)
        if(!isFinal(block.getTransaction(idx), height, blk->time))
            throw Error("Contains a non-final transaction");
    
    _verifier.reset();
    
    insertBlockHeader(blk.count(), block);
    
    // commit transactions
    int64 fees = 0;
    int64 min_fee = 0;
    bool verify = _verification_depth && (height > _verification_depth);
    for(size_t idx = 1; idx < block.getNumTransactions(); ++idx) {
        Transaction txn = block.getTransaction(idx);
        uint256 hash = txn.getHash();
        if (unconfirmed.count(hash) || _claims.have(hash)) // if the transaction is already among the unconfirmed, we have verified it earlier
            verify = false;
        postTransaction(txn, fees, min_fee, blk, idx, verify);
        unconfirmed.erase(hash);
        confirmed.insert(hash);
    }
    // post subsidy - means adding the new coinbase to spendings and the matured coinbase (100 blocks old) to spendables
    Transaction txn = block.getTransaction(0);
    postSubsidy(txn, blk, fees);
    
    if (!_verifier.yield_success())
        throw Error("Verify Signature failed with: " + _verifier.reason());    
}

void BlockChain::detach(BlockIterator &blk, Txns& unconfirmed) {
    Block block;
    getBlock(blk.count(), block);
    rollbackBlock(blk.count()); // this will also remove spendable coins and immature_coinbases
    Transactions& txns = block.getTransactions();
    for (Transactions::const_iterator tx = txns.begin(); tx != txns.end(); ++tx)
        unconfirmed[tx->getHash()] = *tx;
    _branches[blk->hash] = block; // store it in the branches map
}

// append(block)
// accept block is more a offer block or block offered: consume(block) - output is a code that falls in 3 categories:
// accepted(added to best chain, reorganized, sidechain...)
// orphan - might be ok, but not able to check yet
// block is considered wrong
// throw: for some reason the block caused the logic to malfunction
void BlockChain::append(const Block &block) {

    uint256 hash = block.getHash();

    Txns unconfirmed;
    Hashes confirmed;
    
    // check if we already have the block:
    BlockIterator blk = _tree.find(hash);
    if (blk != _tree.end())
        throw Error("Block already accepted");

    BlockIterator prev = _tree.find(block.getPrevBlock());
    
    // do the version check: If a super-majority of blocks within the last 1000 blocks are of version N type - reject blocks of versions below this
    if (block.getVersion() < getMinAcceptedBlockVersion())
        throw Error("Rejected version = " + lexical_cast<string>(block.getVersion())+ " block: version too old.");
    
    if (prev == _tree.end())
        throw Error("Cannot accept orphan block");
    
    if (block.getBits() != _chain.nextWorkRequired(prev))
        throw Error("Incorrect proof of work");
    
    if (block.getBlockTime() <= getMedianTimePast(prev))
        throw Error("Block's timestamp is too early");

    int prev_height = prev.height(); // we need to store this as the prev iterator will be invalid after insert.
    
    BlockTree::Changes changes = _tree.insert(BlockRef(block.getVersion(), hash, prev->hash, block.getBlockTime(), block.getBits()));
    // keep a snapshot of the spendables trie if we need to rollback, however, don't use it during download.
    Spendables snapshot = _spendables;

    if (prev_height < _chain.totalBlocksEstimate() && changes.inserted.size() == 0)
        throw Error("Branching disallowed before last checkpoint at: " + lexical_cast<string>(_chain.totalBlocksEstimate()));
    
    _branches[hash] = block;

    if (changes.inserted.size() == 0)
        return;
    
    try {
        query("BEGIN --BLOCK");

        // now we need to check if the insertion of the new block will change the work
        // note that a change set is like a patch - it contains the blockrefs to remove and the blockrefs to add

        // loop over deleted blocks and detach them
        for (BlockTree::Hashes::const_iterator h = changes.deleted.begin(); h != changes.deleted.end(); ++h) {
            BlockIterator blk = _tree.find(*h);
            detach(blk, unconfirmed);
        }
        
        // loop over inserted blocks and attach them
        for (int i = changes.inserted.size() - 1; i >= 0; --i) {
            blk = _tree.find(changes.inserted[i]);
            attach(blk, unconfirmed, confirmed);
        }
        
        // purge spendings in old blocks - we can just as well serve other nodes with blocks if we have them (requires lazy purging)
        if (!_lazy_purging && blk.count() >= _purge_depth) { // no need to purge during download as we don't store spendings anyway
            query("DELETE FROM Spendings WHERE icnf IN (SELECT cnf FROM Confirmations WHERE count <= ?)", _purge_depth);
            query("DELETE FROM Confirmations WHERE count <= ?", _purge_depth);
        }
        
        // Check that the block is conforming to its block version constraints
        int min_enforced_version = getMinEnforcedBlockVersion();
        switch (min_enforced_version > 0 ? min_enforced_version : 0) {
            default:
            case 3:
                if (block.getVersion() >= 3 && block.checkSpendablesRootInCoinbase(_spendables.root()->hash()))
                    throw Error("Version 3(or more) block with wrong or missing Spendable Root hash in coinbase rejected!");
            case 2:
                if (block.getVersion() >= 2 && block.checkHeightInCoinbase(prev_height + 1))
                    throw Error("Version 2(or more) block with wrong or missing height in coinbase rejected!");
            case 1: // nothing to enforce
            case 0: // nothing to enforce
                break;
        }
        
        // if we reach here, everything went as it should and we can commit.
        query("COMMIT --BLOCK");    
        
        // we have a commit - also commit the transactions to the merkle trie, which happens automatically when spendables goes out of scope
        
        // delete inserted blocks from the _branches
        for (BlockTree::Hashes::const_iterator h = changes.inserted.begin(); h != changes.inserted.end(); ++h)
            _branches.erase(*h);
        updateBestLocator();
    }
    catch (std::exception& e) {
        query("ROLLBACK --BLOCK");
        _tree.pop_back();
        for (BlockTree::Hashes::const_iterator h = changes.deleted.begin(); h != changes.deleted.end(); ++h)
            _branches.erase(*h);

        _spendables = snapshot; // this will restore the Merkle Trie to its former state
        
        throw Error(string("append(Block): ") + e.what());
    }

    // switch on validation if we have more blocks than the validation depth
    if (_validation_depth > 0)
        _spendables.authenticated(_tree.count() >= _validation_depth);
    
    // Erase claims that have now been confirmed in a block
    for (Hashes::const_iterator h = confirmed.begin(); h != confirmed.end(); ++h)
        _claims.erase(*h);

    // delete all transactions more than 24 hrs old
    _claims.purge(GetTime() - 24*60*60);
    
    // Claim transactions that didn't make it into a block, however, don't vaste time verifying, as we have done so already
    for (Txns::iterator tx = unconfirmed.begin(); tx != unconfirmed.end(); ++tx)
        claim(tx->second, false);
    
    log_info("ACCEPT: New best=%s  height=%d", blk->hash.toString().substr(0,20), prev_height + 1);
    if ((prev_height + 1)%1000 == 0) {
        log_info(statistics());
        log_info(_spendables.statistics());
        log_info("Redeem: %s", _redeemStats.str());
        log_info("Issue: %s", _issueStats.str());
        log_info("Signature verification time: %f.3s", 0.000001*_verifySignatureTimer);
        if (_spendables.root())
            log_info("This MerkleTrie Hash: %s", _spendables.root()->hash().toString());
    }

}


void BlockChain::outputPerformanceTimings() const {
    log_info("Performance timings: accept %d, addTo %.2f%%, setBest %.2f%%, connect %.2f%%, verify %.2f%%", _acceptBlockTimer/1000000, 100.*_addToBlockIndexTimer/_acceptBlockTimer, 100.*_setBestChainTimer/_acceptBlockTimer, 100.*_connectInputsTimer/_acceptBlockTimer, 100.*_verifySignatureTimer/_acceptBlockTimer );
}

// update best locator 
void BlockChain::updateBestLocator() {
    vector<int> heights;
    heights.push_back(_tree.height());
    int step = 1;
    // push back 10 heights, then double the steps until genesis is reached
    for (int i = 0;; i++) {
        if (heights.back() - step <= 0) break;
        heights.push_back(heights.back()-step);
        if (heights.size() > 10)
            step *= 2;
    }

    _bestLocator.have.clear();
    for (int i = 0; i < heights.size(); ++i) {
        BlockIterator blk = iterator(heights[i]);
        _bestLocator.have.push_back(blk->hash);
    }
    _bestLocator.have.push_back(getGenesisHash());
}

const BlockLocator& BlockChain::getBestLocator() const {
    return _bestLocator;
}

int BlockChain::getDistanceBack(const BlockLocator& locator) const
{
    // lock the pool and chain for reading
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);
    // Retrace how far back it was in the sender's branch
    int distance = 0;
    int step = 1;
    for (vector<uint256>::const_iterator hash = locator.have.begin(); hash != locator.have.end(); ++hash) {
        BlockIterator blk = _tree.find(*hash);
        if (blk != _tree.end())
            return distance;
        distance += step;
        if (distance > 10)
            step *= 2;
    }
    return distance;
}

void BlockChain::getBlock(BlockIterator blk, Block& block) const {
    // lock the pool and chain for reading
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);
    block.setNull();
    getBlock(blk.count(), block);
}

void BlockChain::getBlock(const uint256 hash, Block& block) const
{
    // lookup in the database:
    BlockIterator blk = _tree.find(hash);
    
    if (!!blk) {
        getBlock(blk, block);
    }
}

void BlockChain::getTransaction(const int64 cnf, Transaction &txn) const {
    Confirmation conf = queryRow<Confirmation(int, unsigned int, int64, int64)>("SELECT (version, locktime, cnf, count) FROM Confirmations WHERE cnf = ?", cnf);
    
    Inputs inputs = queryColRow<Input(uint256, unsigned int, Script, unsigned int)>("SELECT (hash, idx, signature, sequence) FROM Spendings WHERE cin = ? ORDER BY idx", cnf);
    Outputs outputs = queryColRow<Output(int64, Script)>("SELECT value, script FROM (SELECT value, script, idx FROM Unspents WHERE ocnf = ?1 UNION SELECT value, script, idx FROM Spendings WHERE icnf = ?1 ORDER BY idx ASC);", cnf);
    txn = conf;
    txn.setInputs(inputs);
    txn.setOutputs(outputs);
}

void BlockChain::getTransaction(const int64 cnf, Transaction &txn, int64& height, int64& time) const {
    Confirmation conf = queryRow<Confirmation(int, unsigned int, int64, int64)>("SELECT (version, locktime, cnf, count) FROM Confirmations WHERE cnf = ?", cnf);
 
    if (conf.count > LOCKTIME_THRESHOLD) {
        height = -1;
        time = conf.count;
    }
    else {
        height = conf.count - 1;
        BlockIterator blk = iterator(conf.count);
        time = blk->time;
    }
    
    Inputs inputs = queryColRow<Input(uint256, unsigned int, Script, unsigned int)>("SELECT (hash, idx, signature, sequence) FROM Spendings WHERE cin = ? ORDER BY idx", cnf);
    Outputs outputs = queryColRow<Output(int64, Script)>("SELECT value, script FROM (SELECT value, script, idx FROM Unspents WHERE ocnf = ?1 UNION SELECT value, script, idx FROM Spendings WHERE icnf = ?1 ORDER BY idx ASC);", cnf);
    txn = conf;
    txn.setInputs(inputs);
    txn.setOutputs(outputs);
}

BlockIterator BlockChain::iterator(const BlockLocator& locator) const {
    // lock the pool and chain for reading
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);
    // Find the first block the caller has in the main chain
    for (vector<uint256>::const_iterator hash = locator.have.begin(); hash != locator.have.end(); ++hash) {
        BlockIterator blk = _tree.find(*hash);
        if (blk != _tree.end())
            return blk;
    }
    return _tree.begin(); // == the genesisblock
}

BlockIterator BlockChain::iterator(const uint256 hash) const {
    // lock the pool and chain for reading
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);
    // Find the first block the caller has in the main chain
    return _tree.find(hash);
}

double BlockChain::getDifficulty(BlockIterator blk) const {
    // Floating point number that is a multiple of the minimum difficulty,
    // minimum difficulty = 1.0.
    
    if(blk == _tree.end()) blk = _tree.best();

    int shift = (blk->bits >> 24) & 0xff;
    
    double diff = (double)0x0000ffff / (double)(blk->bits & 0x00ffffff);
    
    while (shift < 29) {
        diff *= 256.0;
        shift++;
    }
    while (shift > 29) {
        diff /= 256.0;
        shift--;
    }
    
    return diff;
}

uint256 BlockChain::getBlockHash(const BlockLocator& locator) const {
    // lock the pool and chain for reading
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);
    // Find the first block the caller has in the main chain
    BlockIterator blk = iterator(locator);

    return blk->hash;
}

bool BlockChain::isInMainChain(const uint256 hash) const {
    // lock the pool and chain for reading
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);

    BlockIterator blk = _tree.find(hash);
    return blk.height() >= 0;
}

int BlockChain::getHeight(const uint256 hash) const
{
    // lock the pool and chain for reading
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);

    BlockIterator blk = _tree.find(hash);
    
    if (blk != _tree.end())
        return abs(blk.height());

    return -1;
}


bool BlockChain::haveTx(uint256 hash, bool must_be_confirmed) const
{
    return (_claims.have(hash));
    return true;
    // There is no index on hash, only on the hash + index - we shall assume that the hash + 0 is at least in the spendings, and hence we need not query for more.
    // Further, if we prune the database (remove spendings) we cannot answer this question (at least not if it is 
    int64 cnf = query<int64>("SELECT ocnf FROM Unspents WHERE hash = ?", hash);
    if (!cnf)
        cnf = query<int64>("SELECT ocnf FROM Spendings WHERE hash = ?", hash);

    if (!must_be_confirmed)
        return cnf;
    
    if (!cnf)
        return false;
    
    int count = query<int>("SELECT count FROM Confirmations WHERE cnf = ?", cnf);
    
    if (count < LOCKTIME_THRESHOLD)
        return true;
    else
        return false;
}

bool BlockChain::isFinal(const Transaction& tx, int nBlockHeight, int64 nBlockTime) const
{
    // Time based nLockTime implemented in 0.1.6
    if (tx.lockTime() == 0)
        return true;
    if (nBlockHeight == 0)
        nBlockHeight = _tree.height();
    if (nBlockTime == 0)
        nBlockTime = GetAdjustedTime();
    if ((int64)tx.lockTime() < (tx.lockTime() < LOCKTIME_THRESHOLD ? (int64)nBlockHeight : nBlockTime))
        return true;
    BOOST_FOREACH(const Input& txin, tx.getInputs())
        if (!txin.isFinal())
            return false;
    return true;
}

bool BlockChain::haveBlock(uint256 hash) const {
    return _tree.find(hash) != _tree.end();
}

void BlockChain::getTransaction(const uint256& hash, Transaction& txn) const {
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);

    int64 cnf = query<int64>("SELECT ocnf FROM Unspents WHERE hash = ? LIMIT 1", txn.getHash());
    
    getTransaction(cnf, txn);
}

void BlockChain::getTransaction(const uint256& hash, Transaction& txn, int64& height, int64& time) const
{
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);

    int64 cnf = query<int64>("SELECT ocnf FROM Unspents WHERE hash = ? LIMIT 1", txn.getHash());

    getTransaction(cnf, txn, height, time);
}

Transactions BlockChain::unconfirmedTransactions() const {
    // lock the pool and chain for reading
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);
    
    Transactions txns;
    std::vector<int64> cnfs = queryCol<int64>("SELECT cnf FROM Confirmations WHERE count > ?", LOCKTIME_THRESHOLD);
    
    for (std::vector<int64>::const_iterator cnf = cnfs.begin(); cnf != cnfs.end(); ++cnf) {
        Transaction txn;
        getTransaction(*cnf, txn);
        txns.push_back(txn);
    }
    
    return txns;
}

bool BlockChain::isSpent(Coin coin) const {
    boost::shared_lock< boost::shared_mutex > lock(_chain_and_pool_access);

    if (_validation_depth == 0) // Database
        return query<int64>("SELECT coin FROM Unspents WHERE hash = ? AND idx = ?", coin.hash, coin.index) == 0;
    else
        return !_spendables.find(coin);
}

void BlockChain::getUnspents(const Script& script, Unspents& unspents, unsigned int before) const {
    // check if we have an index of this:
    if (!script_to_unspents())
        throw Error("Lookup of unspents requires an INDEX!");
    
    unspents = queryColRow<Unspent(int64, uint256, unsigned int, int64, Script, int64, int64)>("SELECT coin, hash, idx, value, script, count, ocnf FROM Unspents WHERE script = ?", script);
    
    if (before == 0 || before > LOCKTIME_THRESHOLD) { // include unconfirmed transactions too
        typedef vector<pair<Coin, Output> > Claimed;
        Claimed claimed = _claims.claimed(script);
        for (Claimed::const_iterator c = claimed.begin(); c != claimed.end(); ++c) {
            unsigned int timestamp = _claims.timestamp(c->first.hash);
            if (!before || timestamp <= before)
                unspents.push_back(Unspent(0, c->first.hash, c->first.index, c->second.value(), c->second.script(), timestamp, 0));
        }
    }
    else { // remove those newer than before
        for (int i = unspents.size() - 1; i >= 0; --i)
            if (unspents[i].count > before)
                unspents.erase(unspents.begin()+i);
    }
    
    // finally remove spents
    for (int i = unspents.size() - 1; i >= 0; --i)
        if (_claims.spent(unspents[i].key))
            unspents.erase(unspents.begin()+i);
    
}

int BlockChain::getMinAcceptedBlockVersion() const {
    size_t quorum = _chain.accept_quorum();
    size_t majority = _chain.accept_majority();
    
    map<int, size_t> bins; // map will initialize size_t using the default initializer size_t() which is 0.
    // iterate backwards in the chain until
    BlockTree::Iterator bi = _tree.best();
    size_t blocks = 0;
    do {
        if (++bins[bi->version] > majority) return bi->version;
        if (++blocks > quorum) {
            map<int, size_t>::const_reverse_iterator i = bins.rbegin();
            size_t count = i->second;
            while (++i != bins.rend()) {
                count += i->second;
                if (count > majority)
                    return i->first;
            }
        }
    } while(--bi != _tree.end());
    
    return 1;
}

int BlockChain::getMinEnforcedBlockVersion() const {
    size_t quorum = _chain.enforce_quorum();
    size_t majority = _chain.enforce_majority();
    map<int, size_t> bins; // map will initialize size_t using the default initializer size_t() which is 0.
                          // iterate backwards in the chain until
    BlockTree::Iterator bi = _tree.best();
    size_t blocks = 0;
    do {
        if (++bins[bi->version] > majority) return bi->version;
        if (++blocks > quorum) {
            map<int, size_t>::const_reverse_iterator i = bins.rbegin();
            size_t count = i->second;
            while (++i != bins.rend()) {
                count += i->second;
                if (count > majority)
                    return i->first;
            }
        }
    } while(--bi != _tree.end());
    
    return 1;
}

// getBlockTemplate returns a block template - i.e. a block that has not yet been mined. An optional list of scripts with rewards given as :
// absolute of reward, absolute of fee, fraction of reward, fraction of fee, denominator
Block BlockChain::getBlockTemplate(Payees payees, Fractions fractions, Fractions fee_fractions) const {
    // sanity check of the input parameters
    if (payees.size() == 0) throw Error("Trying the generate a Block Template with no payees");
    if (fractions.size() > 0 && fractions.size() != payees.size()) throw Error("Fractions should be either 0 or match the number of payees");
    if (fee_fractions.size() > 0 && fee_fractions.size() != payees.size()) throw Error("Fee fractions should be either 0 or match the number of payees and fractions");
    
    const int version = 3; // version 3 stores the block height as well as the merkle trie root hash in the coinbase
    const int timestamp = GetTime();
    const unsigned int bits = _chain.nextWorkRequired(_tree.best());
    const int nonce = 0;
    Block block(version, _tree.best()->hash, uint256(0), timestamp, bits, nonce);
    
    // now get the optimal set of transactions
    typedef vector<Transaction> Txns;
    int64 fee = 0;
    Txns txns = _claims.transactions(fee);

    Spendables spendables = _spendables;
    for (Txns::const_iterator tx = txns.begin(); tx != txns.end(); ++tx) {
        uint256 hash = tx->getHash();
        for (size_t idx = 0; idx < tx->getNumOutputs(); ++idx) {
            const Output& output = tx->getOutput(idx);
            spendables.insert(Unspent(0, hash, idx, output.value(), output.script(), 0, 0));
        }
        
        for (Inputs::const_iterator i = tx->getInputs().begin(); i != tx->getInputs().end(); ++i)
            spendables.remove(i->prevout());
    }
    
    // insert the matured coinbase from block #-100
    int count = _tree.count();
    if (count > 0) {
        Unspents coinbase_unspents = queryColRow<Unspent(int64, uint256, unsigned int, int64, Script, int64, int64)>("SELECT coin, hash, idx, value, script, count, ocnf FROM Unspents WHERE count = ?", -(count-COINBASE_MATURITY));
        
        for (Unspents::const_iterator cb = coinbase_unspents.begin(); cb != coinbase_unspents.end(); ++cb)
            spendables.insert(*cb);
    }
    
    uint256 spendables_hash = spendables.root()->hash();
    // insert the coinbase:
    // * calculate the merkle trie root hash
    // * if we are creating a block for distributed mining use that chain to determine the coinbase output

    Script coinbase;
    coinbase << count << spendables_hash;
    Transaction coinbase_txn;
    coinbase_txn.addInput(Input(Coin(), coinbase));

    // and then the outputs!
    // we have the list of payees and their shares
    // first sum up the denominators
    int64 denominator = accumulate(fractions.begin(), fractions.end(), 0);
    int64 fee_denominator = accumulate(fee_fractions.begin(), fee_fractions.end(), 0);
    if (denominator == 0) denominator = payees.size();
    if (fee_denominator == 0) fee_denominator = denominator;

    int64 subsidy = _chain.subsidy(count);
    for (size_t i = 0; i < payees.size(); ++i) {
        int64 nominator = fractions.size() ? fractions[i] : 1;
        int64 fee_nominator = fee_fractions.size() ? fee_fractions[i] : nominator;
        int64 value = nominator*subsidy/denominator + fee_nominator*fee/fee_denominator;
        if (i == 0) value += subsidy%denominator + fee%fee_denominator;
        coinbase_txn.addOutput(Output(value, payees[i]));
    }

    block.addTransaction(coinbase_txn);
    
    for (Transactions::const_iterator tx = txns.begin(); tx != txns.end(); ++tx)
        block.addTransaction(*tx);
    
    return block;
}
