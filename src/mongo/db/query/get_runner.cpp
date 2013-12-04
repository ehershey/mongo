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

#include "mongo/db/query/get_runner.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/query/cached_plan_runner.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/eof_runner.h"
#include "mongo/db/query/idhack_runner.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/multi_plan_runner.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/single_solution_runner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    // Copied verbatim from queryutil.cpp.
    static bool isSimpleIdQuery(const BSONObj& query) {
        // Just one field name.
        BSONObjIterator it(query);
        if (!it.more()) { return false; }

        BSONElement elt = it.next();
        if (it.more()) { return false; }

        // Which is _id...
        if (strcmp("_id", elt.fieldName()) != 0) {
            return false;
        }

        // And not something like { _id : { $gt : ...
        if (elt.isSimpleType()) { return true; }

        // And if the value is an object...
        if (elt.type() == Object) {
            // Can't do this.
            return elt.Obj().firstElementFieldName()[0] != '$';
        }

        return false;
    }

    static bool canUseIDHack(const CanonicalQuery& query) {
        return !query.getParsed().isExplain()
            && !query.getParsed().showDiskLoc()
            && isSimpleIdQuery(query.getParsed().getFilter())
            && !query.getParsed().hasOption(QueryOption_CursorTailable);
    }

    /**
     * For a given query, get a runner.  The runner could be a SingleSolutionRunner, a
     * CachedQueryRunner, or a MultiPlanRunner, depending on the cache/query solver/etc.
     */
    Status getRunner(CanonicalQuery* rawCanonicalQuery, Runner** out, size_t plannerOptions) {
        verify(rawCanonicalQuery);
        auto_ptr<CanonicalQuery> canonicalQuery(rawCanonicalQuery);

        // Try to look up a cached solution for the query.
        // TODO: Can the cache have negative data about a solution?
        PlanCache* localCache = PlanCache::get(canonicalQuery->ns());
        if (NULL != localCache) {
            CachedSolution* cs = localCache->get(*canonicalQuery);
            if (NULL != cs) {
                // We have a cached solution.  Hand the canonical query and cached solution off to
                // the cached plan runner, which takes ownership of both.
                WorkingSet* ws;
                PlanStage* root;
                verify(StageBuilder::build(*cs->solution, &root, &ws));
                *out = new CachedPlanRunner(canonicalQuery.release(), cs, root, ws);
                return Status::OK();
            }
        }

        // No entry in cache for the query.  We have to solve the query ourself.

        // Get the indices that we could possibly use.
        Database* db = cc().database();
        verify( db );
        Collection* collection = db->getCollection( canonicalQuery->ns() );

        // This can happen as we're called by internal clients as well.
        if (NULL == collection) {
            const string& ns = canonicalQuery->ns();
            *out = new EOFRunner(canonicalQuery.release(), ns);
            return Status::OK();
        }
        verify( collection );

        NamespaceDetails* nsd = collection->details();

        // If we have an _id index we can use the idhack runner.
        if (canUseIDHack(*canonicalQuery) && (-1 != nsd->findIdIndex())) {
            *out = new IDHackRunner(collection, canonicalQuery.release());
            return Status::OK();
        }

        // If it's not NULL, we may have indices.  Access the catalog and fill out IndexEntry(s)
        QueryPlannerParams plannerParams;
        for (int i = 0; i < nsd->getCompletedIndexCount(); ++i) {
            IndexDescriptor* desc = collection->getIndexCatalog()->getDescriptor( i );
            plannerParams.indices.push_back(IndexEntry(desc->keyPattern(),
                                                       desc->isMultikey(),
                                                       desc->isSparse(),
                                                       desc->indexName()));
        }

        // Tailable: If the query requests tailable the collection must be capped.
        if (canonicalQuery->getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!nsd->isCapped()) {
                return Status(ErrorCodes::BadValue,
                              "tailable cursor requested on non capped collection");
            }

            // If a sort is specified it must be equal to expectedSort.
            const BSONObj expectedSort = BSON("$natural" << 1);
            const BSONObj& actualSort = canonicalQuery->getParsed().getSort();
            if (!actualSort.isEmpty() && !(actualSort == expectedSort)) {
                return Status(ErrorCodes::BadValue,
                              "invalid sort specified for tailable cursor: "
                              + actualSort.toString());
            }
        }

        // Process the planning options.
        plannerParams.options = plannerOptions;
        if (storageGlobalParams.noTableScan) {
            const string& ns = canonicalQuery->ns();
            // There are certain cases where we ignore this restriction:
            bool ignore = canonicalQuery->getQueryObj().isEmpty()
                          || (string::npos != ns.find(".system."))
                          || (0 == ns.find("local."));
            if (!ignore) {
                plannerParams.options |= QueryPlannerParams::NO_TABLE_SCAN;
            }
        }

        if (!(plannerParams.options & QueryPlannerParams::NO_TABLE_SCAN)) {
            plannerParams.options |= QueryPlannerParams::INCLUDE_COLLSCAN;
        }

        // If the caller wants a shard filter, make sure we're actually sharded.
        if (plannerParams.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            CollectionMetadataPtr collMetadata = shardingState.getCollectionMetadata(canonicalQuery->ns());
            if (collMetadata) {
                plannerParams.shardKey = collMetadata->getKeyPattern();
            }
            else {
                // If there's no metadata don't bother w/the shard filter since we won't know what
                // the key pattern is anyway...
                plannerParams.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
            }
        }

        vector<QuerySolution*> solutions;
        QueryPlanner::plan(*canonicalQuery, plannerParams, &solutions);

        /*
        for (size_t i = 0; i < solutions.size(); ++i) {
            QLOG() << "solution " << i << " is " << solutions[i]->toString() << endl;
        }
        */

        // We cannot figure out how to answer the query.  Should this ever happen?
        if (0 == solutions.size()) {
            return Status(ErrorCodes::BadValue, "No query solutions");
        }

        if (1 == solutions.size()) {
            // Only one possible plan.  Run it.  Build the stages from the solution.
            WorkingSet* ws;
            PlanStage* root;
            verify(StageBuilder::build(*solutions[0], &root, &ws));

            // And, run the plan.
            *out = new SingleSolutionRunner(canonicalQuery.release(), solutions[0], root, ws);
            return Status::OK();
        }
        else {
            // Many solutions.  Let the MultiPlanRunner pick the best, update the cache, and so on.
            auto_ptr<MultiPlanRunner> mpr(new MultiPlanRunner(canonicalQuery.release()));
            for (size_t i = 0; i < solutions.size(); ++i) {
                WorkingSet* ws;
                PlanStage* root;
                verify(StageBuilder::build(*solutions[i], &root, &ws));
                // Takes ownership of all arguments.
                mpr->addPlan(solutions[i], root, ws);
            }
            *out = mpr.release();
            return Status::OK();
        }
    }

    DeregisterEvenIfUnderlyingCodeThrows::~DeregisterEvenIfUnderlyingCodeThrows() {
        ClientCursor::deregisterRunner(_runner);
    }

}  // namespace mongo
