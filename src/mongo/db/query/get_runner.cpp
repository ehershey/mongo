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

#include <limits>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/query/cached_plan_runner.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/eof_runner.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/idhack_runner.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/multi_plan_runner.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/single_solution_runner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/index_names.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    MONGO_EXPORT_SERVER_PARAMETER(enableIndexIntersection, bool, true);

    static bool canUseIDHack(const CanonicalQuery& query) {
        return !query.getParsed().isExplain()
            && !query.getParsed().showDiskLoc()
            && CanonicalQuery::isSimpleIdQuery(query.getParsed().getFilter())
            && !query.getParsed().hasOption(QueryOption_CursorTailable);
    }

    // static
    void filterAllowedIndexEntries(const AllowedIndices& allowedIndices,
                                   std::vector<IndexEntry>* indexEntries) {
        invariant(indexEntries);

        // Filter index entries
        // Check BSON objects in AllowedIndices::_indexKeyPatterns against IndexEntry::keyPattern.
        // Removes IndexEntrys that do not match _indexKeyPatterns.
        std::vector<IndexEntry> temp;
        for (std::vector<IndexEntry>::const_iterator i = indexEntries->begin();
             i != indexEntries->end(); ++i) {
            const IndexEntry& indexEntry = *i;
            for (std::vector<BSONObj>::const_iterator j = allowedIndices.indexKeyPatterns.begin();
                 j != allowedIndices.indexKeyPatterns.end(); ++j) {
                const BSONObj& index = *j;
                // Copy index entry to temp vector if found in query settings.
                if (0 == indexEntry.keyPattern.woCompare(index)) {
                    temp.push_back(indexEntry);
                    break;
                }
            }
        }

        // Update results.
        temp.swap(*indexEntries);
    }

    /**
     * For a given query, get a runner.  The runner could be a SingleSolutionRunner, a
     * CachedQueryRunner, or a MultiPlanRunner, depending on the cache/query solver/etc.
     */
    Status getRunner(CanonicalQuery* rawCanonicalQuery,
                     Runner** out, size_t plannerOptions) {
        verify(rawCanonicalQuery);
        Database* db = cc().database();
        verify(db);
        return getRunner(db->getCollection(rawCanonicalQuery->ns()),
                         rawCanonicalQuery,
                         out,
                         plannerOptions);
    }

    Status getRunner(Collection* collection,
                     const std::string& ns,
                     const BSONObj& unparsedQuery,
                     Runner** outRunner,
                     CanonicalQuery** outCanonicalQuery,
                     size_t plannerOptions) {

        if (!collection) {
            *outCanonicalQuery = NULL;
            *outRunner = new EOFRunner(NULL, ns);
            return Status::OK();
        }
        if (!CanonicalQuery::isSimpleIdQuery(unparsedQuery) ||
            !collection->getIndexCatalog()->findIdIndex()) {

            Status status = CanonicalQuery::canonicalize(
                    collection->ns(),
                    unparsedQuery,
                    outCanonicalQuery);
            if (!status.isOK())
                return status;
            return getRunner(collection, *outCanonicalQuery, outRunner, plannerOptions);
        }

        *outCanonicalQuery = NULL;
        *outRunner = new IDHackRunner(collection, unparsedQuery["_id"].wrap());
        return Status::OK();
    }

    namespace {
        // The body is below in the "count hack" section but getRunner calls it.
        bool turnIxscanIntoCount(QuerySolution* soln);
    }  // namespace

    /**
     * For a given query, get a runner.  The runner could be a SingleSolutionRunner, a
     * CachedQueryRunner, or a MultiPlanRunner, depending on the cache/query solver/etc.
     */
    Status getRunner(Collection* collection,
                     CanonicalQuery* rawCanonicalQuery,
                     Runner** out,
                     size_t plannerOptions) {

        verify(rawCanonicalQuery);
        auto_ptr<CanonicalQuery> canonicalQuery(rawCanonicalQuery);

        // This can happen as we're called by internal clients as well.
        if (NULL == collection) {
            const string& ns = canonicalQuery->ns();
            *out = new EOFRunner(canonicalQuery.release(), ns);
            return Status::OK();
        }

        // If we have an _id index we can use the idhack runner.
        if (canUseIDHack(*canonicalQuery) && collection->getIndexCatalog()->findIdIndex()) {
            *out = new IDHackRunner(collection, canonicalQuery.release());
            return Status::OK();
        }

        // If it's not NULL, we may have indices.  Access the catalog and fill out IndexEntry(s)
        QueryPlannerParams plannerParams;

        IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(false);
        while (ii.more()) {
            const IndexDescriptor* desc = ii.next();
            plannerParams.indices.push_back(IndexEntry(desc->keyPattern(),
                                                       desc->isMultikey(),
                                                       desc->isSparse(),
                                                       desc->indexName(),
                                                       desc->infoObj()));
        }

        // If query supports index filters, filter params.indices by indices in query settings.
        QuerySettings* querySettings = collection->infoCache()->getQuerySettings();
        AllowedIndices* allowedIndicesRaw;

        // Filter index catalog if index filters are specified for query.
        // Also, signal to planner that application hint should be ignored.
        if (querySettings->getAllowedIndices(*canonicalQuery, &allowedIndicesRaw)) {
            boost::scoped_ptr<AllowedIndices> allowedIndices(allowedIndicesRaw);
            filterAllowedIndexEntries(*allowedIndices, &plannerParams.indices);
            plannerParams.indexFiltersApplied = true;
        }

        // Tailable: If the query requests tailable the collection must be capped.
        if (canonicalQuery->getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!collection->isCapped()) {
                return Status(ErrorCodes::BadValue,
                              "error processing query: " + canonicalQuery->toString() +
                              " tailable cursor requested on non capped collection");
            }

            // If a sort is specified it must be equal to expectedSort.
            const BSONObj expectedSort = BSON("$natural" << 1);
            const BSONObj& actualSort = canonicalQuery->getParsed().getSort();
            if (!actualSort.isEmpty() && !(actualSort == expectedSort)) {
                return Status(ErrorCodes::BadValue,
                              "error processing query: " + canonicalQuery->toString() +
                              " invalid sort specified for tailable cursor: "
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
            CollectionMetadataPtr collMetadata =
                shardingState.getCollectionMetadata(canonicalQuery->ns());

            if (collMetadata) {
                plannerParams.shardKey = collMetadata->getKeyPattern();
            }
            else {
                // If there's no metadata don't bother w/the shard filter since we won't know what
                // the key pattern is anyway...
                plannerParams.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
            }
        }

        // Try to look up a cached solution for the query.
        //
        // Skip cache look up for non-cacheable queries.
        // See PlanCache::shouldCacheQuery()
        //
        // TODO: Can the cache have negative data about a solution?
        CachedSolution* rawCS;
        if (PlanCache::shouldCacheQuery(*canonicalQuery) &&
            collection->infoCache()->getPlanCache()->get(*canonicalQuery, &rawCS).isOK()) {
            // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
            boost::scoped_ptr<CachedSolution> cs(rawCS);
            QuerySolution *qs, *backupQs;
            Status status = QueryPlanner::planFromCache(*canonicalQuery, plannerParams, *cs,
                                                        &qs, &backupQs);

            // See SERVER-12438. Unfortunately we have to defer to the backup solution
            // if both a batch size is set and a sort is requested.
            //
            // TODO: it would be really nice to delete this block in the future.
            if (status.isOK() && NULL != backupQs &&
                0 < canonicalQuery->getParsed().getNumToReturn() &&
                !canonicalQuery->getParsed().getSort().isEmpty()) {
                delete qs;

                WorkingSet* ws;
                PlanStage* root;
                verify(StageBuilder::build(*backupQs, &root, &ws));

                // And, run the plan.
                *out = new SingleSolutionRunner(collection,
                                                canonicalQuery.release(),
                                                backupQs, root, ws);
                return Status::OK();
            }

            if (status.isOK()) {
                if (plannerParams.options & QueryPlannerParams::PRIVATE_IS_COUNT) {
                    if (turnIxscanIntoCount(qs)) {
                        WorkingSet* ws;
                        PlanStage* root;
                        verify(StageBuilder::build(*qs, &root, &ws));
                        *out = new SingleSolutionRunner(collection,
                                                        canonicalQuery.release(), qs, root, ws);
                        if (NULL != backupQs) {
                            delete backupQs;
                        }
                        return Status::OK();
                    }
                }

                WorkingSet* ws;
                PlanStage* root;
                verify(StageBuilder::build(*qs, &root, &ws));
                CachedPlanRunner* cpr = new CachedPlanRunner(collection,
                                                             canonicalQuery.release(), qs,
                                                             root, ws);

                if (NULL != backupQs) {
                    WorkingSet* backupWs;
                    PlanStage* backupRoot;
                    verify(StageBuilder::build(*backupQs, &backupRoot, &backupWs));
                    cpr->setBackupPlan(backupQs, backupRoot, backupWs);
                }

                *out = cpr;
                return Status::OK();
            }
        }

        if (enableIndexIntersection) {
            plannerParams.options |= QueryPlannerParams::INDEX_INTERSECTION;
        }

        plannerParams.options |= QueryPlannerParams::KEEP_MUTATIONS;

        vector<QuerySolution*> solutions;
        Status status = QueryPlanner::plan(*canonicalQuery, plannerParams, &solutions);
        if (!status.isOK()) {
            return Status(ErrorCodes::BadValue,
                          "error processing query: " + canonicalQuery->toString() +
                          " planner returned error: " + status.reason());
        }

        // We cannot figure out how to answer the query.  Perhaps it requires an index
        // we do not have?
        if (0 == solutions.size()) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                          << "error processing query: "
                          << canonicalQuery->toString()
                          << " No query solutions");
        }

        // See if one of our solutions is a fast count hack in disguise.
        if (plannerParams.options & QueryPlannerParams::PRIVATE_IS_COUNT) {
            for (size_t i = 0; i < solutions.size(); ++i) {
                if (turnIxscanIntoCount(solutions[i])) {
                    // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
                    for (size_t j = 0; j < solutions.size(); ++j) {
                        if (j != i) {
                            delete solutions[j];
                        }
                    }

                    // We're not going to cache anything that's fast count.
                    WorkingSet* ws;
                    PlanStage* root;
                    verify(StageBuilder::build(*solutions[i], &root, &ws));
                    *out = new SingleSolutionRunner(collection,
                                                    canonicalQuery.release(),
                                                    solutions[i],
                                                    root,
                                                    ws);
                    return Status::OK();
                }
            }
        }

        if (1 == solutions.size()) {
            // Only one possible plan.  Run it.  Build the stages from the solution.
            WorkingSet* ws;
            PlanStage* root;
            verify(StageBuilder::build(*solutions[0], &root, &ws));

            // And, run the plan.
            *out = new SingleSolutionRunner(collection,
                                            canonicalQuery.release(),solutions[0], root, ws);
            return Status::OK();
        }
        else {
            // See SERVER-12438. In an ideal world we should not arbitrarily prefer certain
            // solutions over others. But unfortunately for historical reasons we are forced
            // to prefer a solution where the index provides the sort, if the batch size
            // is set and a sort is requested. Read SERVER-12438 for details, if you dare.
            //
            // TODO: it would be really nice to delete this entire block in the future.
            if (0 < canonicalQuery->getParsed().getNumToReturn()
                && !canonicalQuery->getParsed().getSort().isEmpty()) {
                // Look for a solution without a blocking sort stage.
                for (size_t i = 0; i < solutions.size(); ++i) {
                    if (!solutions[i]->hasSortStage) {
                        WorkingSet* ws;
                        PlanStage* root;
                        verify(StageBuilder::build(*solutions[i], &root, &ws));

                        // Free unused solutions.
                        for (size_t j = 0; j < solutions.size(); ++j) {
                            if (j != i) {
                                delete solutions[j];
                            }
                        }

                        // And, run the plan.
                        *out = new SingleSolutionRunner(collection,
                                                       canonicalQuery.release(),
                                                       solutions[i], root, ws);
                        return Status::OK();
                    }
                }
            }

            // Many solutions.  Let the MultiPlanRunner pick the best, update the cache, and so on.
            auto_ptr<MultiPlanRunner> mpr(new MultiPlanRunner(collection,canonicalQuery.release()));

            for (size_t i = 0; i < solutions.size(); ++i) {
                WorkingSet* ws;
                PlanStage* root;
                if (solutions[i]->cacheData.get()) {
                    solutions[i]->cacheData->indexFilterApplied = plannerParams.indexFiltersApplied;
                }
                verify(StageBuilder::build(*solutions[i], &root, &ws));
                // Takes ownership of all arguments.
                mpr->addPlan(solutions[i], root, ws);
            }
            *out = mpr.release();
            return Status::OK();
        }
    }

    //
    // Count hack
    //

    namespace {

        /**
         * Returns 'true' if the bounds 'bounds' can be represented as an interval between two the two
         * values 'startKey' and 'endKey'.  Inclusivity of each bound is set through the relevant
         * fooKeyInclusive parameter.
         *
         * Returns 'false' otherwise.
         *
         * XXX: unit test this.
         */
        bool isSingleInterval(const IndexBounds& bounds,
                              BSONObj* startKey,
                              bool* startKeyInclusive,
                              BSONObj* endKey,
                              bool* endKeyInclusive) {
            // We build our start/end keys as we go.
            BSONObjBuilder startBob;
            BSONObjBuilder endBob;

            // The start and end keys are inclusive unless we have a non-point interval, in which case
            // we take the inclusivity from there.
            *startKeyInclusive = true;
            *endKeyInclusive = true;

            size_t fieldNo = 0;

            // First, we skip over point intervals.
            for (; fieldNo < bounds.fields.size(); ++fieldNo) {
                const OrderedIntervalList& oil = bounds.fields[fieldNo];
                // A point interval requires just one interval...
                if (1 != oil.intervals.size()) {
                    break;
                }
                if (!oil.intervals[0].isPoint()) {
                    break;
                }
                // Since it's a point, start == end.
                startBob.append(oil.intervals[0].start);
                endBob.append(oil.intervals[0].end);
            }

            if (fieldNo >= bounds.fields.size()) {
                // All our intervals are points.  We count for all values of one field.
                *startKey = startBob.obj();
                *endKey = endBob.obj();
                return true;
            }

            // After point intervals we can have exactly one non-point interval.
            const OrderedIntervalList& nonPoint = bounds.fields[fieldNo];
            if (1 != nonPoint.intervals.size()) {
                return false;
            }

            // Add the non-point interval to our builder and set the inclusivity from it.
            startBob.append(nonPoint.intervals[0].start);
            *startKeyInclusive = nonPoint.intervals[0].startInclusive;
            endBob.append(nonPoint.intervals[0].end);
            *endKeyInclusive = nonPoint.intervals[0].endInclusive;

            ++fieldNo;

            // Get some "all values" intervals for comparison's sake.
            // TODO: make static?
            Interval minMax = IndexBoundsBuilder::allValues();
            Interval maxMin = minMax;
            maxMin.reverse();

            // And after the non-point interval we can have any number of "all values" intervals.
            for (; fieldNo < bounds.fields.size(); ++fieldNo) {
                const OrderedIntervalList& oil = bounds.fields[fieldNo];
                // "All Values" is just one point.
                if (1 != oil.intervals.size()) {
                    break;
                }

                // Must be min->max or max->min.
                if (oil.intervals[0].equals(minMax)) {
                    // As an example for the logic below, consider the index {a:1, b:1} and a count for
                    // {a: {$gt: 2}}.  Our start key isn't inclusive (as it's $gt: 2) and looks like
                    // {"":2} so far.  If we move to the key greater than {"":2, "": MaxKey} we will get
                    // the first value of 'a' that is greater than 2.
                    if (!*startKeyInclusive) {
                        startBob.appendMaxKey("");
                    }
                    else {
                        // In this case, consider the index {a:1, b:1} and a count for {a:{$gte: 2}}.
                        // We want to look at all values where a is 2, so our start key is {"":2,
                        // "":MinKey}.
                        startBob.appendMinKey("");
                    }

                    // Same deal as above.  Consider the index {a:1, b:1} and a count for {a: {$lt: 2}}.
                    // Our end key isn't inclusive as ($lt: 2) and looks like {"":2} so far.  We can't
                    // look at any values where a is 2 so we have to stop at {"":2, "": MinKey} as
                    // that's the smallest key where a is still 2.
                    if (!*endKeyInclusive) {
                        endBob.appendMinKey("");
                    }
                    else {
                        endBob.appendMaxKey("");
                    }
                }
                else if (oil.intervals[0].equals(maxMin)) {
                    // The reasoning here is the same as above but with the directions reversed.
                    if (!*startKeyInclusive) {
                        startBob.appendMinKey("");
                    }
                    else {
                        startBob.appendMaxKey("");
                    }
                    if (!*endKeyInclusive) {
                        endBob.appendMaxKey("");
                    }
                    else {
                        endBob.appendMinKey("");
                    }
                }
                else {
                    // No dice.
                    break;
                }
            }

            if (fieldNo >= bounds.fields.size()) {
                *startKey = startBob.obj();
                *endKey = endBob.obj();
                return true;
            }
            else {
                return false;
            }
        }

        /**
         * Returns 'true' if the provided solution 'soln' can be rewritten to use
         * a fast counting stage.  Mutates the tree in 'soln->root'.
         *
         * Otherwise, returns 'false'.
         */
        bool turnIxscanIntoCount(QuerySolution* soln) {
            QuerySolutionNode* root = soln->root.get();

            // Root should be a fetch w/o any filters.
            if (STAGE_FETCH != root->getType()) {
                return false;
            }

            if (NULL != root->filter.get()) {
                return false;
            }

            // Child should be an ixscan.
            if (STAGE_IXSCAN != root->children[0]->getType()) {
                return false;
            }

            IndexScanNode* isn = static_cast<IndexScanNode*>(root->children[0]);

            // No filters allowed and side-stepping isSimpleRange for now.  TODO: do we ever see
            // isSimpleRange here?  because we could well use it.  I just don't think we ever do see it.
            if (NULL != isn->filter.get() || isn->bounds.isSimpleRange) {
                return false;
            }

            // Make sure the bounds are OK.
            BSONObj startKey;
            bool startKeyInclusive;
            BSONObj endKey;
            bool endKeyInclusive;

            if (!isSingleInterval(isn->bounds, &startKey, &startKeyInclusive, &endKey, &endKeyInclusive)) {
                return false;
            }

            // Make the count node that we replace the fetch + ixscan with.
            CountNode* cn = new CountNode();
            cn->indexKeyPattern = isn->indexKeyPattern;
            cn->startKey = startKey;
            cn->startKeyInclusive = startKeyInclusive;
            cn->endKey = endKey;
            cn->endKeyInclusive = endKeyInclusive;
            // Takes ownership of 'cn' and deletes the old root.
            soln->root.reset(cn);
            return true;
        }

        /**
         * Returns true if indices contains an index that can be
         * used with DistinctNode. Sets indexOut to the array index
         * of PlannerParams::indices.
         * Look for the index for the fewest fields.
         * Criteria for suitable index is that the index cannot be special
         * (geo, hashed, text, ...).
         */
        bool getDistinctNodeIndex(const std::vector<IndexEntry>& indices, size_t* indexOut) {
            invariant(indexOut);
            int minFields = std::numeric_limits<int>::max();
            for (size_t i = 0; i < indices.size(); ++i) {
                // Skip special indices.
                if (!IndexNames::findPluginName(indices[i].keyPattern).empty()) {
                    continue;
                }
                int nFields = indices[i].keyPattern.nFields();
                // Pick the index with the lowest number of fields.
                if (nFields < minFields) {
                    minFields = nFields;
                    *indexOut = i;
                }
            }
            return minFields != std::numeric_limits<int>::max();
        }
            
    }  // namespace

    Status getRunnerCount(Collection* collection,
                          const BSONObj& query,
                          const BSONObj& hintObj,
                          Runner** out) {
        verify(collection);

        CanonicalQuery* cq;
        uassertStatusOK(CanonicalQuery::canonicalize(collection->ns().ns(),
                                                     query,
                                                     BSONObj(),
                                                     BSONObj(), 
                                                     0,
                                                     0,
                                                     hintObj,
                                                     &cq)); 

        return getRunner(collection, cq, out, QueryPlannerParams::PRIVATE_IS_COUNT);
    }

    //
    // Distinct hack
    //

    /**
     * If possible, turn the provided QuerySolution into a QuerySolution that uses a DistinctNode
     * to provide results for the distinct command.
     *
     * If the provided solution could be mutated successfully, returns true, otherwise returns
     * false.
     */
    bool turnIxscanIntoDistinctIxscan(QuerySolution* soln, const string& field) {
        QuerySolutionNode* root = soln->root.get();

        // We're looking for a project on top of an ixscan.
        if (STAGE_PROJECTION == root->getType() && (STAGE_IXSCAN == root->children[0]->getType())) {
            IndexScanNode* isn = static_cast<IndexScanNode*>(root->children[0]);

            // An additional filter must be applied to the data in the key, so we can't just skip
            // all the keys with a given value; we must examine every one to find the one that (may)
            // pass the filter.
            if (NULL != isn->filter.get()) {
                return false;
            }

            // We only set this when we have special query modifiers (.max() or .min()) or other
            // special cases.  Don't want to handle the interactions between those and distinct.
            // Don't think this will ever really be true but if it somehow is, just ignore this
            // soln.
            if (isn->bounds.isSimpleRange) {
                return false;
            }

            // Make a new DistinctNode.  We swap this for the ixscan in the provided solution.
            DistinctNode* dn = new DistinctNode();
            dn->indexKeyPattern = isn->indexKeyPattern;
            dn->direction = isn->direction;
            dn->bounds = isn->bounds;

            // Figure out which field we're skipping to the next value of.  TODO: We currently only
            // try to distinct-hack when there is an index prefixed by the field we're distinct-ing
            // over.  Consider removing this code if we stick with that policy.
            dn->fieldNo = 0;
            BSONObjIterator it(isn->indexKeyPattern);
            while (it.more()) {
                if (field == it.next().fieldName()) {
                    break;
                }
                dn->fieldNo++;
            }

            // Delete the old index scan, set the child of project to the fast distinct scan.
            delete root->children[0];
            root->children[0] = dn;
            return true;
        }

        return false;
    }

    Status getRunnerDistinct(Collection* collection,
                             const BSONObj& query,
                             const string& field,
                             Runner** out) {
        // This should'a been checked by the distinct command.
        verify(collection);

        // TODO: check for idhack here?

        // When can we do a fast distinct hack?
        // 1. There is a plan with just one leaf and that leaf is an ixscan.
        // 2. The ixscan indexes the field we're interested in.
        // 2a: We are correct if the index contains the field but for now we look for prefix.
        // 3. The query is covered/no fetch.
        //
        // We go through normal planning (with limited parameters) to see if we can produce
        // a soln with the above properties.

        QueryPlannerParams plannerParams;
        plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;

        IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(false);
        while (ii.more()) {
            const IndexDescriptor* desc = ii.next();
            // The distinct hack can work if any field is in the index but it's not always clear
            // if it's a win unless it's the first field.
            if (desc->keyPattern().firstElement().fieldName() == field) {
                plannerParams.indices.push_back(IndexEntry(desc->keyPattern(),
                                                           desc->isMultikey(),
                                                           desc->isSparse(),
                                                           desc->indexName(),
                                                           desc->infoObj()));
            }
        }

        // We only care about the field that we're projecting over.  Have to drop the _id field
        // explicitly because those are .find() semantics.
        //
        // Applying a projection allows the planner to try to give us covered plans.
        BSONObj projection;
        if ("_id" == field) {
            projection = BSON("_id" << 1);
        }
        else {
            projection = BSON("_id" << 0 << field << 1);
        }

        // Apply a projection of the key.  Empty BSONObj() is for the sort.
        CanonicalQuery* cq;
        Status status = CanonicalQuery::canonicalize(collection->ns().ns(), query, BSONObj(), projection, &cq);
        if (!status.isOK()) {
            return status;
        }

        // No index has the field we're looking for.  Punt to normal planning.
        if (plannerParams.indices.empty()) {
            // Takes ownership of cq.
            return getRunner(cq, out);
        }

        // If we're here, we have an index prefixed by the field we're distinct-ing over.

        // If there's no query, we can just distinct-scan one of the indices.
        // Not every index in plannerParams.indices may be suitable. Refer to
        // getDistinctNodeIndex().
        size_t distinctNodeIndex = 0;
        if (query.isEmpty() &&
            getDistinctNodeIndex(plannerParams.indices, &distinctNodeIndex)) {
            DistinctNode* dn = new DistinctNode();
            dn->indexKeyPattern = plannerParams.indices[distinctNodeIndex].keyPattern;
            dn->direction = 1;
            IndexBoundsBuilder::allValuesBounds(dn->indexKeyPattern, &dn->bounds);
            dn->fieldNo = 0;

            QueryPlannerParams params;

            // Takes ownership of 'dn'.
            QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(*cq, params, dn);
            verify(soln);

            WorkingSet* ws;
            PlanStage* root;
            verify(StageBuilder::build(*soln, &root, &ws));
            *out = new SingleSolutionRunner(collection, cq, soln, root, ws);
            return Status::OK();
        }

        // See if we can answer the query in a fast-distinct compatible fashion.
        vector<QuerySolution*> solutions;
        status = QueryPlanner::plan(*cq, plannerParams, &solutions);
        if (!status.isOK()) {
            return getRunner(cq, out);
        }

        // XXX: why do we need to do this?  planner should prob do this internally
        cq->root()->resetTag();

        // We look for a solution that has an ixscan we can turn into a distinctixscan
        for (size_t i = 0; i < solutions.size(); ++i) {
            if (turnIxscanIntoDistinctIxscan(solutions[i], field)) {
                // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
                for (size_t j = 0; j < solutions.size(); ++j) {
                    if (j != i) {
                        delete solutions[j];
                    }
                }

                // Build and return the SSR over solutions[i].
                WorkingSet* ws;
                PlanStage* root;
                verify(StageBuilder::build(*solutions[i], &root, &ws));
                *out = new SingleSolutionRunner(collection, cq, solutions[i], root, ws);
                return Status::OK();
            }
        }

        // If we're here, the planner made a soln with the restricted index set but we couldn't
        // translate any of them into a distinct-compatible soln.  So, delete the solutions and just
        // go through normal planning.
        for (size_t i = 0; i < solutions.size(); ++i) {
            delete solutions[i];
        }

        return getRunner(cq, out);
    }

    ScopedRunnerRegistration::ScopedRunnerRegistration(Runner* runner)
        : _runner(runner) {
        // Collection can be null for EOFRunner, or other places where registration is not needed
        if ( _runner->collection() )
            _runner->collection()->cursorCache()->registerRunner( runner );
    }

    ScopedRunnerRegistration::~ScopedRunnerRegistration() {
        if ( _runner->collection() )
            _runner->collection()->cursorCache()->deregisterRunner( _runner );
    }

}  // namespace mongo
