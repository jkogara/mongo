// drop_indexes.cpp

/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/structure/collection.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    /* "dropIndexes" is now the preferred form - "deleteIndexes" deprecated */
    class CmdDropIndexes : public Command {
    public:
        virtual bool logTheOp() {
            return true;
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream& help ) const {
            help << "drop indexes for a collection";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::dropIndex);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname+".system.indexes";
            std::string toDeleteNs = dbname+"."+cmdObj.firstElement().valuestr();
            BSONObjBuilder builder;
            builder.append("ns", systemIndexes);
            builder.append("op", "insert");
            builder.append("insert.ns", toDeleteNs);

            // Get index name to drop
            BSONElement toDrop = cmdObj.getField("index");

            if (toDrop.type() == String) {
                // Kill all in-progress indexes
                if (strcmp("*", toDrop.valuestr()) == 0) {
                    BSONObj criteria = builder.done();
                    return IndexBuilder::killMatchingIndexBuilds(criteria);
                }
                // Kill an in-progress index by name
                else {
                    builder.append("insert.name", toDrop.valuestr());
                    BSONObj criteria = builder.done();
                    return IndexBuilder::killMatchingIndexBuilds(criteria);
                }
            }
            // Kill an in-progress index build by index key
            else if (toDrop.type() == Object) {
                builder.append("insert.key", toDrop.Obj());
                BSONObj criteria = builder.done();
                return IndexBuilder::killMatchingIndexBuilds(criteria);
            }

            return std::vector<BSONObj>();
        }

        CmdDropIndexes() : Command("dropIndexes", false, "deleteIndexes") { }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& anObjBuilder, bool /*fromRepl*/) {
            BSONElement e = jsobj.firstElement();
            string toDeleteNs = dbname + '.' + e.valuestr();
            if (!serverGlobalParams.quiet) {
                MONGO_TLOG(0) << "CMD: dropIndexes " << toDeleteNs << endl;
            }

            Collection* collection = cc().database()->getCollection( toDeleteNs );
            if ( ! collection ) {
                errmsg = "ns not found";
                return false;
            }

            stopIndexBuilds(dbname, jsobj);

            IndexCatalog* indexCatalog = collection->getIndexCatalog();
            anObjBuilder.appendNumber("nIndexesWas", indexCatalog->numIndexesTotal() );


            BSONElement f = jsobj.getField("index");
            if ( f.type() == String ) {

                string indexToDelete = f.valuestr();

                if ( indexToDelete == "*" ) {
                    Status s = indexCatalog->dropAllIndexes( false );
                    if ( !s.isOK() ) {
                        appendCommandStatus( anObjBuilder, s );
                        return false;
                    }
                    anObjBuilder.append("msg", "non-_id indexes dropped for collection");
                    return true;
                }

                int idxNo = collection->details()->findIndexByName( indexToDelete );
                if ( idxNo < 0 ) {
                    errmsg = str::stream() << "index not found with name [" << indexToDelete << "]";
                    return false;
                }

                if ( idxNo == collection->details()->findIdIndex() ) {
                    errmsg = "cannot drop _id index";
                    return false;
                }

                Status s = indexCatalog->dropIndex( idxNo );
                if ( !s.isOK() ) {
                    appendCommandStatus( anObjBuilder, s );
                    return false;
                }

                return true;
            }

            if ( f.type() == Object ) {
                int idxNo = collection->details()->findIndexByKeyPattern( f.embeddedObject() );
                if ( idxNo < 0 ) {
                    errmsg = "can't find index with key:";
                    errmsg += f.embeddedObject().toString();
                    return false;
                }

                if ( idxNo == collection->details()->findIdIndex() ) {
                    errmsg = "cannot drop _id index";
                    return false;
                }

                Status s = indexCatalog->dropIndex( idxNo );
                if ( !s.isOK() ) {
                    appendCommandStatus( anObjBuilder, s );
                    return false;
                }

                return true;
            }

            errmsg = "invalid index name spec";
            return false;
        }

    } cmdDropIndexes;

    class CmdReIndex : public Command {
    public:
        virtual bool logTheOp() { return false; } // only reindexes on the one node
        virtual bool slaveOk() const { return true; }    // can reindex on a secondary
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream& help ) const {
            help << "re-index a collection";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::reIndex);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }
        CmdReIndex() : Command("reIndex") { }

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname, 
                                                     const BSONObj& cmdObj) {
            std::string systemIndexes = dbname + ".system.indexes";
            std::string ns = dbname + '.' + cmdObj["reIndex"].valuestrsafe();
            BSONObj criteria = BSON("ns" << systemIndexes << "op" << "insert" << "insert.ns" << ns);

            return IndexBuilder::killMatchingIndexBuilds(criteria);
        }

        bool run(const string& dbname , BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool /*fromRepl*/) {
            static DBDirectClient db;

            BSONElement e = jsobj.firstElement();
            string toDeleteNs = dbname + '.' + e.valuestr();

            MONGO_TLOG(0) << "CMD: reIndex " << toDeleteNs << endl;

            Collection* collection = cc().database()->getCollection( toDeleteNs );

            if ( !collection ) {
                errmsg = "ns not found";
                return false;
            }

            BackgroundOperation::assertNoBgOpInProgForNs( toDeleteNs );

            std::vector<BSONObj> indexesInProg = stopIndexBuilds(dbname, jsobj);

            list<BSONObj> all;
            auto_ptr<DBClientCursor> i = db.query( dbname + ".system.indexes" , BSON( "ns" << toDeleteNs ) , 0 , 0 , 0 , QueryOption_SlaveOk );
            BSONObjBuilder b;
            while ( i->more() ) {
                BSONObj o = i->next().removeField("v").getOwned();
                b.append( BSONObjBuilder::numStr( all.size() ) , o );
                all.push_back( o );
            }
            result.appendNumber( "nIndexesWas", collection->getIndexCatalog()->numIndexesTotal() );

            Status s = collection->getIndexCatalog()->dropAllIndexes( true );
            if ( !s.isOK() ) {
                errmsg = "dropIndexes failed";
                return appendCommandStatus( result, s );
            }

            for ( list<BSONObj>::iterator i=all.begin(); i!=all.end(); i++ ) {
                BSONObj o = *i;
                LOG(1) << "reIndex ns: " << toDeleteNs << " index: " << o << endl;
                Status s = collection->getIndexCatalog()->createIndex( o, false );
                if ( !s.isOK() )
                    return appendCommandStatus( result, s );
            }

            result.append( "nIndexes" , (int)all.size() );
            result.appendArray( "indexes" , b.obj() );

            IndexBuilder::restoreIndexes(indexesInProg);
            return true;
        }
    } cmdReIndex;


}
