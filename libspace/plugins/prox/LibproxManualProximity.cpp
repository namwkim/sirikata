// Copyright (c) 2011 Sirikata Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE file.


#include "LibproxManualProximity.hpp"
#include "Options.hpp"
#include <sirikata/core/options/CommonOptions.hpp>

#include "Protocol_Prox.pbj.hpp"

#include <sirikata/core/command/Commander.hpp>
#include <json_spirit/json_spirit.h>
#include <boost/foreach.hpp>

#include <prox/manual/RTreeManualQueryHandler.hpp>

#include <sirikata/core/network/IOStrandImpl.hpp>

namespace Sirikata {

#define PROXLOG(level,msg) SILOG(prox,level,msg)

using std::tr1::placeholders::_1;
using std::tr1::placeholders::_2;


LibproxManualProximity::LibproxManualProximity(SpaceContext* ctx, LocationService* locservice, CoordinateSegmentation* cseg, SpaceNetwork* net, AggregateManager* aggmgr)
 : LibproxProximityBase(ctx, locservice, cseg, net, aggmgr),
   mOHQueries(),
   mOHHandlerPoller(mProxStrand, std::tr1::bind(&LibproxManualProximity::tickQueryHandler, this, mOHQueryHandler), "LibproxManualProximity ObjectHost Handler Poll", Duration::milliseconds((int64)100))
{

    // OH Queries
    for(int i = 0; i < NUM_OBJECT_CLASSES; i++) {
        if (i >= mNumQueryHandlers) {
            mOHQueryHandler[i].handler = NULL;
            continue;
        }
        mOHQueryHandler[i].handler = new Prox::RTreeManualQueryHandler<ObjectProxSimulationTraits>(10);
        mOHQueryHandler[i].handler->setAggregateListener(this); // *Must* be before handler->initialize
        bool object_static_objects = (mSeparateDynamicObjects && i == OBJECT_CLASS_STATIC);
        mOHQueryHandler[i].handler->initialize(
            mLocCache, mLocCache,
            object_static_objects, false /* not replicated */,
            std::tr1::bind(&LibproxManualProximity::handlerShouldHandleObject, this, object_static_objects, false, _1, _2, _3, _4, _5, _6)
        );
    }
}

LibproxManualProximity::~LibproxManualProximity() {
    for(int i = 0; i < NUM_OBJECT_CLASSES; i++) {
        delete mOHQueryHandler[i].handler;
    }
}

void LibproxManualProximity::start() {
    LibproxProximityBase::start();

    mContext->add(&mOHHandlerPoller);
}

void LibproxManualProximity::poll() {
    // Update server-to-server angles if necessary
    sendQueryRequests();

    // Get and ship OH results
    std::deque<OHResult> oh_results_copy;
    mOHResults.swap(oh_results_copy);
    mOHResultsToSend.insert(mOHResultsToSend.end(), oh_results_copy.begin(), oh_results_copy.end());

    while(!mOHResultsToSend.empty()) {
        const OHResult& msg_front = mOHResultsToSend.front();
        sendObjectHostResult(OHDP::NodeID(msg_front.first), msg_front.second);
        delete msg_front.second;
        mOHResultsToSend.pop_front();
    }

}

void LibproxManualProximity::addQuery(UUID obj, SolidAngle sa, uint32 max_results) {
    // Ignored, this query handler only deals with ObjectHost queries
}

void LibproxManualProximity::addQuery(UUID obj, const String& params) {
    // Ignored, this query handler only deals with ObjectHost queries
}

void LibproxManualProximity::removeQuery(UUID obj) {
    // Ignored, this query handler only deals with ObjectHost queries
}


// Note: LocationServiceListener interface is only used in order to get updates on objects which have
// registered queries, allowing us to update those queries as appropriate.  All updating of objects
// in the prox data structure happens via the LocationServiceCache
void LibproxManualProximity::localObjectRemoved(const UUID& uuid, bool agg) {
    mProxStrand->post(
        std::tr1::bind(&LibproxManualProximity::removeStaticObjectTimeout, this, uuid),
        "LibproxManualProximity::removeStaticObjectTimeout"
    );
}
void LibproxManualProximity::localLocationUpdated(const UUID& uuid, bool agg, const TimedMotionVector3f& newval) {
    if (mSeparateDynamicObjects)
        checkObjectClass(true, uuid, newval);
}
void LibproxManualProximity::replicaObjectRemoved(const UUID& uuid) {
    mProxStrand->post(
        std::tr1::bind(&LibproxManualProximity::removeStaticObjectTimeout, this, uuid),
        "LibproxManualProximity::removeStaticObjectTimeout"
    );
}
void LibproxManualProximity::replicaLocationUpdated(const UUID& uuid, const TimedMotionVector3f& newval) {
    if (mSeparateDynamicObjects)
        checkObjectClass(false, uuid, newval);
}


// Migration management

std::string LibproxManualProximity::migrationClientTag() {
    return "prox";
}

std::string LibproxManualProximity::generateMigrationData(const UUID& obj, ServerID source_server, ServerID dest_server) {
    // There shouldn't be any object data to move since we only manage
    // ObjectHost queries
    return "";
}

void LibproxManualProximity::receiveMigrationData(const UUID& obj, ServerID source_server, ServerID dest_server, const std::string& data) {
    // We should never be receiving data for migrations since we only
    // handle object host queries
    assert(data.empty());
}



// MAIN Thread -- Aggregate server-to-server queries and top-level events

void LibproxManualProximity::sendQueryRequests() {
    ServerSet sub_servers;
    getServersForAggregateQueryUpdate(&sub_servers);
    for(ServerSet::const_iterator it = sub_servers.begin(); it != sub_servers.end(); it++) {
        ServerID sid = *it;
        PROXLOG(warn, "Ignoring request to send aggregate query update to server " << sid << " because manual queries don't support server-to-server queries yet.");
        // if failed: addServerForAggregateQueryUpdate(sid);
    }

}

// MAIN Thread -- Object host session and message management

void LibproxManualProximity::onObjectHostSession(const OHDP::NodeID& id, ObjectHostSessionPtr oh_sess) {
    // Setup listener for requests from object hosts. We should only
    // have one active substream at a time. Proximity sessions are
    // always initiated by the object host -- upon receiving a
    // connection we register the query and use the same substream to
    // transmit results.
    // We also pass through the seqNoPtr() since we need to extract it in this
    // thread, it shouldn't change, and we don't want to hold onto the session.
    oh_sess->stream()->listenSubstream(
        OBJECT_PORT_PROXIMITY,
        std::tr1::bind(
            &LibproxManualProximity::handleObjectHostSubstream, this,
            _1, _2, oh_sess->seqNoPtr()
        )
    );
}

void LibproxManualProximity::handleObjectHostSubstream(int success, OHDPSST::Stream::Ptr substream, SeqNoPtr seqno) {
    if (success != SST_IMPL_SUCCESS) return;

    PROXLOG(detailed, "New object host proximity session from " << substream->remoteEndPoint().endPoint);
    // Store this for sending data back
    addObjectHostProxStreamInfo(substream);
    // And register to read requests
    readFramesFromObjectHostStream(
        substream->remoteEndPoint().endPoint.node(),
        mProxStrand->wrap(
            std::tr1::bind(&LibproxManualProximity::handleObjectHostProxMessage, this, substream->remoteEndPoint().endPoint.node(), _1, seqno)
        )
    );
}

void LibproxManualProximity::onObjectHostSessionEnded(const OHDP::NodeID& id) {
    mProxStrand->post(
        std::tr1::bind(&LibproxManualProximity::handleObjectHostSessionEnded, this, id),
        "LibproxManualProximity::handleObjectHostSessionEnded"
    );
}


int32 LibproxManualProximity::objectHostQueries() const {
    return mOHQueries[OBJECT_CLASS_STATIC].size();
}






// PROX Thread

void LibproxManualProximity::aggregateCreated(ProxAggregator* handler, const UUID& objid) {
    // We ignore aggregates built of dynamic objects, they aren't useful for
    // creating aggregate meshes
    if (!static_cast<ProxQueryHandler*>(handler)->staticOnly()) return;
    LibproxProximityBase::aggregateCreated(objid);
}

void LibproxManualProximity::aggregateChildAdded(ProxAggregator* handler, const UUID& objid, const UUID& child, const Vector3f& bnds_center, const float32 bnds_center_radius, const float32 max_obj_size) {
    if (!static_cast<ProxQueryHandler*>(handler)->staticOnly()) return;
    LibproxProximityBase::aggregateChildAdded(objid, child, bnds_center, AggregateBoundingInfo(Vector3f::zero(), bnds_center_radius, max_obj_size));
}

void LibproxManualProximity::aggregateChildRemoved(ProxAggregator* handler, const UUID& objid, const UUID& child, const Vector3f& bnds_center, const float32 bnds_center_radius, const float32 max_obj_size) {
    if (!static_cast<ProxQueryHandler*>(handler)->staticOnly()) return;
    LibproxProximityBase::aggregateChildRemoved(objid, child, bnds_center, AggregateBoundingInfo(Vector3f::zero(), bnds_center_radius, max_obj_size));
}

void LibproxManualProximity::aggregateBoundsUpdated(ProxAggregator* handler, const UUID& objid, const Vector3f& bnds_center, const float32 bnds_center_radius, const float32 max_obj_size) {
    if (!static_cast<ProxQueryHandler*>(handler)->staticOnly()) return;
    LibproxProximityBase::aggregateBoundsUpdated(objid, bnds_center, AggregateBoundingInfo(Vector3f::zero(), bnds_center_radius, max_obj_size));
}

void LibproxManualProximity::aggregateDestroyed(ProxAggregator* handler, const UUID& objid) {
    if (!static_cast<ProxQueryHandler*>(handler)->staticOnly()) return;
    LibproxProximityBase::aggregateDestroyed(objid);
}

void LibproxManualProximity::aggregateObserved(ProxAggregator* handler, const UUID& objid, uint32 nobservers) {
    if (!static_cast<ProxQueryHandler*>(handler)->staticOnly()) return;
    LibproxProximityBase::aggregateObserved(objid, nobservers);
}


void LibproxManualProximity::tickQueryHandler(ProxQueryHandlerData qh[NUM_OBJECT_CLASSES]) {
    // Not really any better place to do this. We'll call this more frequently
    // than necessary by putting it here, but hopefully it doesn't matter since
    // most of the time nothing will be done.
    processExpiredStaticObjectTimeouts();

    // We need to actually swap any objects that the previous step
    // found. However, we need to be careful because just performing
    // the addObject() and removeObject() can result in incorrect
    // results: because each class is ticked separately we could do
    // the addition and removal, then tick the handlers in the wrong
    // order such that querier q which already has object o in the
    // result set gets messages [add o, remove o] when they really
    // needed to get [remove o, add o].
    //
    // To handle this, we just do all the removals, perform a tick,
    // then do all the additions. This forces this step to only
    // generate removals, then lets the next tick generate the
    // additions.

    Time simT = mContext->simTime();
    for(int i = 0; i < NUM_OBJECT_CLASSES; i++) {
        if (qh[i].handler != NULL) {
            for(ObjectIDSet::iterator it = qh[i].removals.begin(); it != qh[i].removals.end(); it++)
                qh[i].handler->removeObject(*it, true);
            qh[i].removals.clear();

            qh[i].handler->tick(simT);

            for(ObjectIDSet::iterator it = qh[i].additions.begin(); it != qh[i].additions.end(); it++)
                qh[i].handler->addObject(*it);
            qh[i].additions.clear();
        }
    }
}


// PROX Thread -- Server-to-server and top-level pinto

void LibproxManualProximity::handleForcedDisconnection(ServerID server) {
    PROXLOG(warn, "Ignoring forced disconnection by server " << server << " since manual queries don't support server-to-server queries yet.");
}


// PROX Thread -- OH queries

void LibproxManualProximity::handleObjectHostProxMessage(const OHDP::NodeID& id, const String& data, SeqNoPtr seqNo) {
    // Handle the seqno update
    if (mOHSeqNos.find(id) == mOHSeqNos.end())
        mOHSeqNos.insert( OHSeqNoInfoMap::value_type(id, seqNo) );

    Protocol::Prox::QueryRequest request;
    bool parse_success = request.ParseFromString(data);

    namespace json = json_spirit;
    json::Value query_params;
    if (!json::read(request.query_parameters(), query_params)) {
        PROXLOG(error, "Error parsing object host query request: " << request.query_parameters());
        return;
    }

    String action = query_params.getString("action", String(""));
    if (action.empty()) return;
    if (action == "init") {
        PROXLOG(detailed, "Init query for " << id);

        for(int i = 0; i < NUM_OBJECT_CLASSES; i++) {
            if (mOHQueryHandler[i].handler == NULL) continue;

            // FIXME we need some way of specifying the basic query
            // parameters for OH queries (or maybe just get rid of
            // these basic properties as they aren't even required for
            // this type of query?)
            TimedMotionVector3f pos(mContext->simTime(), MotionVector3f(Vector3f(0, 0, 0), Vector3f(0, 0, 0)));
            BoundingSphere3f bounds(Vector3f(0, 0, 0), 0);
            float max_size = 0.f;
            ProxQuery* q = mOHQueryHandler[i].handler->registerQuery(pos, bounds, max_size);
            mOHQueries[i][id] = q;
            mInvertedOHQueries[q] = id;
            // Set the listener last since it can trigger callbacks
            // and we want everything to be setup already
            q->setEventListener(this);
        }
    }
    else if (action == "refine") {
        PROXLOG(detailed, "Refine query for " << id);

        if (!query_params.contains("nodes") || !query_params.get("nodes").isArray()) {
            PROXLOG(detailed, "Invalid refine request " << id);
            return;
        }
        json::Array json_nodes = query_params.getArray("nodes");
        std::vector<UUID> refine_nodes;
        BOOST_FOREACH(json::Value& v, json_nodes) {
            if (!v.isString()) return;
            refine_nodes.push_back(UUID(v.getString(), UUID::HumanReadable()));
        }

        for(int kls = 0; kls < NUM_OBJECT_CLASSES; kls++) {
            if (mOHQueryHandler[kls].handler == NULL) continue;
            OHQueryMap::iterator query_it = mOHQueries[kls].find(id);
            if (query_it == mOHQueries[kls].end()) continue;
            ProxQuery* q = query_it->second;

            for(uint32 i = 0; i < refine_nodes.size(); i++)
                q->refine(refine_nodes[i]);
        }
    }
    else if (action == "coarsen") {
        PROXLOG(detailed, "Coarsen query for " << id);

        if (!query_params.contains("nodes") || !query_params.get("nodes").isArray()) {
            PROXLOG(detailed, "Invalid coarsen request " << id);
            return;
        }
        json::Array json_nodes = query_params.getArray("nodes");
        std::vector<UUID> coarsen_nodes;
        BOOST_FOREACH(json::Value& v, json_nodes) {
            if (!v.isString()) return;
            coarsen_nodes.push_back(UUID(v.getString(), UUID::HumanReadable()));
        }

        for(int kls = 0; kls < NUM_OBJECT_CLASSES; kls++) {
            if (mOHQueryHandler[kls].handler == NULL) continue;
            OHQueryMap::iterator query_it = mOHQueries[kls].find(id);
            if (query_it == mOHQueries[kls].end()) continue;
            ProxQuery* q = query_it->second;

            for(uint32 i = 0; i < coarsen_nodes.size(); i++)
                q->coarsen(coarsen_nodes[i]);
        }
    }
    else if (action == "destroy") {
        destroyQuery(id);
    }
}

void LibproxManualProximity::handleObjectHostSessionEnded(const OHDP::NodeID& id) {
    destroyQuery(id);
}

void LibproxManualProximity::destroyQuery(const OHDP::NodeID& id) {
    PROXLOG(detailed, "Destroy query for " << id);
    for(int i = 0; i < NUM_OBJECT_CLASSES; i++) {
        if (mOHQueryHandler[i].handler == NULL) continue;

        OHQueryMap::iterator it = mOHQueries[i].find(id);
        if (it == mOHQueries[i].end()) continue;

        ProxQuery* q = it->second;
        mOHQueries[i].erase(it);
        mInvertedOHQueries.erase(q);
        delete q; // Note: Deleting query notifies QueryHandler and unsubscribes.
    }

    eraseSeqNoInfo(id);
    mContext->mainStrand->post(
        std::tr1::bind(&LibproxManualProximity::handleRemoveAllOHLocSubscription, this, id),
        "LibproxManualProximity::handleRemoveAllOHLocSubscription"
    );

}



bool LibproxManualProximity::handlerShouldHandleObject(bool is_static_handler, bool is_global_handler, const UUID& obj_id, bool is_local, bool is_aggregate, const TimedMotionVector3f& pos, const BoundingSphere3f& region, float maxSize) {
    // We just need to decide whether the query handler should handle
    // the object. We need to consider local vs. replica and static
    // vs. dynamic.  All must 'vote' for handling the object for us to
    // say it should be handled, so as soon as we find a negative
    // response we can return false.

    // First classify by local vs. replica. Only say no on a local
    // handler looking at a replica.
    if (!is_local && !is_global_handler) return false;

    // If we're not doing the static/dynamic split, then this is a non-issue
    if (!mSeparateDynamicObjects) return true;

    // If we are splitting them, check velocity against is_static_handler. The
    // value here as arbitrary, just meant to indicate such small movement that
    // the object is effectively
    bool is_static = velocityIsStatic(pos.velocity());
    if ((is_static && is_static_handler) ||
        (!is_static && !is_static_handler))
        return true;
    else
        return false;
}


void LibproxManualProximity::handleCheckObjectClassForHandlers(const UUID& objid, bool is_static, ProxQueryHandlerData handlers[NUM_OBJECT_CLASSES]) {
    if ( (is_static && handlers[OBJECT_CLASS_STATIC].handler->containsObject(objid)) ||
        (!is_static && handlers[OBJECT_CLASS_DYNAMIC].handler->containsObject(objid)) )
        return;

    // Validate that the other handler has the object.
    assert(
        (is_static && handlers[OBJECT_CLASS_DYNAMIC].handler->containsObject(objid)) ||
        (!is_static && handlers[OBJECT_CLASS_STATIC].handler->containsObject(objid))
    );

    // If it wasn't in the right place, switch it.
    int swap_out = is_static ? OBJECT_CLASS_DYNAMIC : OBJECT_CLASS_STATIC;
    int swap_in = is_static ? OBJECT_CLASS_STATIC : OBJECT_CLASS_DYNAMIC;
    PROXLOG(debug, "Swapping " << objid.toString() << " from " << ObjectClassToString((ObjectClass)swap_out) << " to " << ObjectClassToString((ObjectClass)swap_in));
    handlers[swap_out].removals.insert(objid);
    handlers[swap_in].additions.insert(objid);
}

void LibproxManualProximity::trySwapHandlers(bool is_local, const UUID& objid, bool is_static) {
    handleCheckObjectClassForHandlers(objid, is_static, mOHQueryHandler);
}



SeqNoPtr LibproxManualProximity::getSeqNoInfo(const OHDP::NodeID& node)
{
    OHSeqNoInfoMap::iterator proxSeqNoIt = mOHSeqNos.find(node);
    assert (proxSeqNoIt != mOHSeqNos.end());
    return proxSeqNoIt->second;
}

void LibproxManualProximity::eraseSeqNoInfo(const OHDP::NodeID& node)
{
    OHSeqNoInfoMap::iterator proxSeqNoIt = mOHSeqNos.find(node);
    if (proxSeqNoIt == mOHSeqNos.end()) return;
    mOHSeqNos.erase(proxSeqNoIt);
}

void LibproxManualProximity::queryHasEvents(ProxQuery* query) {
    uint32 max_count = GetOptionValue<uint32>(PROX_MAX_PER_RESULT);

    OHDP::NodeID query_id = mInvertedOHQueries[query];
    SeqNoPtr seqNoPtr = getSeqNoInfo(query_id);

    QueryEventList evts;
    query->popEvents(evts);

    PROXLOG(detailed, evts.size() << " events for query " << query_id);
    while(!evts.empty()) {
        Sirikata::Protocol::Prox::ProximityResults prox_results;
        prox_results.set_t(mContext->simTime());

        uint32 count = 0;
        while(count < max_count && !evts.empty()) {
            const ProxQueryEvent& evt = evts.front();
            Sirikata::Protocol::Prox::IProximityUpdate event_results = prox_results.add_update();

            // We always want to tag this with the unique query handler index ID
            // so the client can properly group the replicas
            Sirikata::Protocol::Prox::IIndexProperties index_props = event_results.mutable_index_properties();
            index_props.set_id(evt.indexID());

            for(uint32 aidx = 0; aidx < evt.additions().size(); aidx++) {
                UUID objid = evt.additions()[aidx].id();
                if (mLocCache->tracking(objid)) { // If the cache already lost it, we can't do anything
                    count++;

                    mContext->mainStrand->post(
                        std::tr1::bind(&LibproxManualProximity::handleAddOHLocSubscriptionWithID, this, query_id, objid, evt.indexID()),
                        "LibproxManualProximity::handleAddOHLocSubscription"
                    );

                    Sirikata::Protocol::Prox::IObjectAddition addition = event_results.add_addition();
                    addition.set_object( objid );


                    //query_id contains the uuid of the object that is receiving
                    //the proximity message that obj_id has been added.
                    uint64 seqNo = (*seqNoPtr)++;
                    addition.set_seqno (seqNo);


                    Sirikata::Protocol::ITimedMotionVector motion = addition.mutable_location();
                    TimedMotionVector3f loc = mLocCache->location(objid);
                    motion.set_t(loc.updateTime());
                    motion.set_position(loc.position());
                    motion.set_velocity(loc.velocity());

                    TimedMotionQuaternion orient = mLocCache->orientation(objid);
                    Sirikata::Protocol::ITimedMotionQuaternion msg_orient = addition.mutable_orientation();
                    msg_orient.set_t(orient.updateTime());
                    msg_orient.set_position(orient.position());
                    msg_orient.set_velocity(orient.velocity());

                    Sirikata::Protocol::IAggregateBoundingInfo msg_bounds = addition.mutable_aggregate_bounds();
                    AggregateBoundingInfo bnds = mLocCache->bounds(objid);
                    msg_bounds.set_center_offset(bnds.centerOffset);
                    msg_bounds.set_center_bounds_radius(bnds.centerBoundsRadius);
                    msg_bounds.set_max_object_size(bnds.maxObjectRadius);

                    const String& mesh = mLocCache->mesh(objid);
                    if (mesh.size() > 0)
                        addition.set_mesh(mesh);
                    const String& phy = mLocCache->physics(objid);
                    if (phy.size() > 0)
                        addition.set_physics(phy);

                    // We should either include the parent ID, or if it's empty,
                    // then this is a root and we should include basic tree
                    // properties. However, we only need to include the details
                    // if this is the first time we're seeing the root, in which
                    // case we'll get a lone addition of the root.
                    UUID parentid = evt.additions()[aidx].parent();
                    if (parentid != UUID::null()) {
                        addition.set_parent(parentid);
                    }
                    else if (/*lone addition*/ aidx == 0 && evt.additions().size() == 1 && evt.removals().size() == 0) {
                        // We need to figure out which query handler this came
                        // from. FIXME currently we can use this simple approach
                        // of just checking the static/dynamic from the OH query
                        // handlers since we're only handling objects on this
                        // server. When we deal with top-level pinto + other
                        // trees, we might need a real index to figure out which
                        // query processor this is coming from.

                        // The tree ID identifies where this tree goes in some
                        // larger structure. In our case it'll be a server ID
                        // indicating which server the objects (and tree) are
                        // replicated from or NullServerID to fit with.
                        // FIXME when we have results from multiple trees (local
                        // objects & against replicated trees) we'll need to
                        // actually figure out what ID is right here. Currently
                        // we only return local results
                        index_props.set_index_id( boost::lexical_cast<String>(mContext->id()) );

                        // And whether it's static or not, which actually also
                        // is important in determining a "full" tree id
                        // (e.g. objects from server A that are dynamic) but
                        // which we want to keep separate and explicit so the
                        // other side can perform optimizations for static
                        // object trees
                        if (query->handler() == mOHQueryHandler[OBJECT_CLASS_STATIC].handler) {
                            index_props.set_dynamic_classification(Sirikata::Protocol::Prox::IndexProperties::Static);
                        }
                        else {
                            assert(query->handler() == mOHQueryHandler[OBJECT_CLASS_DYNAMIC].handler);
                            index_props.set_dynamic_classification(Sirikata::Protocol::Prox::IndexProperties::Dynamic);
                        }
                    }
                    addition.set_type(
                        (evt.additions()[aidx].type() == ProxQueryEvent::Normal) ?
                        Sirikata::Protocol::Prox::ObjectAddition::Object :
                        Sirikata::Protocol::Prox::ObjectAddition::Aggregate
                    );
                }
            }
            for(uint32 ridx = 0; ridx < evt.removals().size(); ridx++) {
                UUID objid = evt.removals()[ridx].id();
                count++;
                // Clear out seqno and let main strand remove loc
                // subcription

                mContext->mainStrand->post(
                    std::tr1::bind(&LibproxManualProximity::handleRemoveOHLocSubscriptionWithID, this, query_id, objid, evt.indexID()),
                    "LibproxManualProximity::handleRemoveOHLocSubscription"
                );

                Sirikata::Protocol::Prox::IObjectRemoval removal = event_results.add_removal();
                removal.set_object( objid );
                uint64 seqNo = (*seqNoPtr)++;
                removal.set_seqno (seqNo);
                removal.set_type(
                    (evt.removals()[ridx].permanent() == ProxQueryEvent::Permanent)
                    ? Sirikata::Protocol::Prox::ObjectRemoval::Permanent
                    : Sirikata::Protocol::Prox::ObjectRemoval::Transient
                );
            }
            evts.pop_front();
        }

        // Note null ID's since these are OHDP messages.
        Sirikata::Protocol::Object::ObjectMessage* obj_msg = createObjectMessage(
            mContext->id(),
            UUID::null(), OBJECT_PORT_PROXIMITY,
            UUID::null(), OBJECT_PORT_PROXIMITY,
            serializePBJMessage(prox_results)
        );
        mOHResults.push( OHResult(query_id, obj_msg) );
    }
}




// Command handlers
void LibproxManualProximity::commandProperties(const Command::Command& cmd, Command::Commander* cmdr, Command::CommandID cmdid) {
    Command::Result result = Command::EmptyResult();

    // Properties
    result.put("name", "libprox-manual");
    result.put("settings.handlers", mNumQueryHandlers);
    result.put("settings.dynamic_separate", mSeparateDynamicObjects);
    if (mSeparateDynamicObjects)
        result.put("settings.static_heuristic", mMoveToStaticDelay.toString());

    // Current state

    // Properties of objects
    int32 oh_query_objects = (mNumQueryHandlers == 2 ? (mOHQueryHandler[0].handler->numObjects() + mOHQueryHandler[1].handler->numObjects()) : mOHQueryHandler[0].handler->numObjects());
    result.put("objects.properties.local_count", oh_query_objects);
    result.put("objects.properties.remote_count", 0);
    result.put("objects.properties.count", oh_query_objects);

    // Properties of queries
    result.put("queries.oh.count", mOHQueries[0].size());
    // Technically not thread safe, but these should be simple
    // read-only accesses.
    uint32 oh_messages = 0;
    for(ObjectHostProxStreamMap::iterator prox_stream_it = mObjectHostProxStreams.begin(); prox_stream_it != mObjectHostProxStreams.end(); prox_stream_it++)
        oh_messages += prox_stream_it->second->outstanding.size();
    result.put("queries.oh.messages", oh_messages);

    cmdr->result(cmdid, result);
}

void LibproxManualProximity::commandListHandlers(const Command::Command& cmd, Command::Commander* cmdr, Command::CommandID cmdid) {
    Command::Result result = Command::EmptyResult();
    for(int i = 0; i < NUM_OBJECT_CLASSES; i++) {
        if (mOHQueryHandler[i].handler != NULL) {
            String key = String("handlers.oh.") + ObjectClassToString((ObjectClass)i) + ".";
            result.put(key + "name", String("oh-queries.") + ObjectClassToString((ObjectClass)i) + "-objects");
            result.put(key + "queries", mOHQueryHandler[i].handler->numQueries());
            result.put(key + "objects", mOHQueryHandler[i].handler->numObjects());
            result.put(key + "nodes", mOHQueryHandler[i].handler->numNodes());
        }
    }
    cmdr->result(cmdid, result);
}

bool LibproxManualProximity::parseHandlerName(const String& name, ProxQueryHandlerData** handlers_out, ObjectClass* class_out) {
    // Should be of the form xxx-queries.yyy-objects, containing only 1 .
    std::size_t dot_pos = name.find('.');
    if (dot_pos == String::npos || name.rfind('.') != dot_pos)
        return false;

    String handler_part = name.substr(0, dot_pos);
    if (handler_part == "oh-queries")
        *handlers_out = mOHQueryHandler;
    else
        return false;

    String class_part = name.substr(dot_pos+1);
    if (class_part == "dynamic-objects")
        *class_out = OBJECT_CLASS_DYNAMIC;
    else if (class_part == "static-objects")
        *class_out = OBJECT_CLASS_STATIC;
    else
        return false;

    return true;
}

void LibproxManualProximity::commandForceRebuild(const Command::Command& cmd, Command::Commander* cmdr, Command::CommandID cmdid) {
    Command::Result result = Command::EmptyResult();

    ProxQueryHandlerData* handlers = NULL;
    ObjectClass klass;
    if (!cmd.contains("handler") ||
        !parseHandlerName(cmd.getString("handler"), &handlers, &klass))
    {
        result.put("error", "Ill-formatted request: handler not specified or invalid.");
        cmdr->result(cmdid, result);
        return;
    }


    result.put("error", "Rebuilding manual proximity processors isn't supported yet.");
    cmdr->result(cmdid, result);
}

void LibproxManualProximity::commandListNodes(const Command::Command& cmd, Command::Commander* cmdr, Command::CommandID cmdid) {
    Command::Result result = Command::EmptyResult();

    ProxQueryHandlerData* handlers = NULL;
    ObjectClass klass;
    if (!cmd.contains("handler") ||
        !parseHandlerName(cmd.getString("handler"), &handlers, &klass))
    {
        result.put("error", "Ill-formatted request: handler not specified or invalid.");
        cmdr->result(cmdid, result);
        return;
    }

    result.put( String("nodes"), Command::Array());
    Command::Array& nodes_ary = result.getArray("nodes");
    for(ProxQueryHandler::NodeIterator nit = handlers[klass].handler->nodesBegin(); nit != handlers[klass].handler->nodesEnd(); nit++) {
        nodes_ary.push_back( Command::Object() );
        nodes_ary.back().put("id", nit.id().toString());
        nodes_ary.back().put("parent", nit.parentId().toString());
        BoundingSphere3f bounds = nit.bounds(mContext->simTime());
        nodes_ary.back().put("bounds.center.x", bounds.center().x);
        nodes_ary.back().put("bounds.center.y", bounds.center().y);
        nodes_ary.back().put("bounds.center.z", bounds.center().z);
        nodes_ary.back().put("bounds.radius", bounds.radius());
        nodes_ary.back().put("cuts", nit.cuts());
    }

    cmdr->result(cmdid, result);
}



} // namespace Sirikata
