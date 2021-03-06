/**
 ******************************************************************************
 *
 * @file       telemetry.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @author     Vladimir Ermakov, Copyright (C) 2013.
 * @brief The UAVTalk protocol
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "telemetry.h"
#include "oplinksettings.h"
#include "objectpersistence.h"
#include <ros/console.h>
#include <stdlib.h>

using namespace openpilot;

/** Constructor
 */
Telemetry::Telemetry(boost::asio::io_service &io, UAVTalk *utalk, UAVObjectManager *objMngr) :
	io_service(io),
	updateTimer(io_service)
{
	this->utalk   = utalk;
	this->objMngr = objMngr;

	// Process all objects in the list
	UAVObjectManager::objects_map objs = objMngr->getObjects();
	for (UAVObjectManager::objects_map::iterator itr = objs.begin(); itr != objs.end(); ++itr) {
		registerObject(itr->second[0]); // we only need to register one instance per object type
	}

	// Listen to new object creations
	objMngr->newObject.connect(boost::bind(&Telemetry::newObject, this, _1));
	objMngr->newInstance.connect(boost::bind(&Telemetry::newInstance, this, _1));
	// Listen to transaction completions
	utalk->transactionCompleted.connect(boost::bind(&Telemetry::transactionCompleted, this, _1, _2));
	// Get GCS stats object
	gcsStatsObj = GCSTelemetryStats::GetInstance(objMngr);
	// Setup and start the periodic timer
	timeToNextUpdateMs = 0;
	updateTimer.expires_from_now(boost::posix_time::seconds(1));
	updateTimer.async_wait(boost::bind(&Telemetry::processPeriodicUpdates, this, boost::asio::placeholders::error));
	// Setup and start the stats timer
	txErrors  = 0;
	txRetries = 0;
}

Telemetry::~Telemetry()
{
	updateTimer.cancel();
	for (std::map<uint32_t, ObjectTransactionInfo *>::iterator itr = transMap.begin(); itr != transMap.end(); ++itr) {
		itr->second->timer.cancel();
		delete itr->second;
	}
}

/** Register a new object for periodic updates (if enabled)
 */
void Telemetry::registerObject(UAVObject *obj)
{
	// Setup object for periodic updates
	addObject(obj);

	// Setup object for telemetry updates
	updateObject(obj, EV_NONE);
}

/** Add an object in the list used for periodic updates
 */
void Telemetry::addObject(UAVObject *obj)
{
	// Check if object type is already in the list
	for (int n = 0; n < objList.size(); ++n) {
		if (objList[n].obj->getObjID() == obj->getObjID()) {
			// Object type (not instance!) is already in the list, do nothing
			return;
		}
	}

	// If this point is reached, then the object type is new, let's add it
	ObjectTimeInfo timeInfo;
	timeInfo.obj = obj;
	timeInfo.timeToNextUpdateMs = 0;
	timeInfo.updatePeriodMs     = 0;
	objList.push_back(timeInfo);
}

/** Update the object's timers
 */
void Telemetry::setUpdatePeriod(UAVObject *obj, int32_t periodMs)
{
	// Find object type (not instance!) and update its period
	for (int n = 0; n < objList.size(); ++n) {
		if (objList[n].obj->getObjID() == obj->getObjID()) {
			objList[n].updatePeriodMs     = periodMs;
			objList[n].timeToNextUpdateMs = uint32_t((float)periodMs * (float)rand() / (float)RAND_MAX); // avoid bunching of updates
		}
	}
}

/** Connect to all instances of an object depending on the event mask specified
 */
void Telemetry::connectToObjectInstances(UAVObject *obj, uint32_t eventMask)
{
	UAVObjectManager::inst_vec objs = objMngr->getObjectInstances(obj->getObjID());
	for (int n = 0; n < objs.size(); ++n) {
		// Disconnect all
		objs[n]->objectUnpacked.disconnect(boost::bind(&Telemetry::objectUnpacked, this, _1));
		objs[n]->objectUpdatedAuto.disconnect(boost::bind(&Telemetry::objectUpdatedAuto, this, _1));
		objs[n]->objectUpdatedManual.disconnect(boost::bind(&Telemetry::objectUpdatedManual, this, _1));
		objs[n]->objectUpdatedPeriodic.disconnect(boost::bind(&Telemetry::objectUpdatedPeriodic, this, _1));
		objs[n]->updateRequested.disconnect(boost::bind(&Telemetry::updateRequested, this, _1));
		// Connect only the selected events
		if ((eventMask & EV_UNPACKED) != 0) {
			objs[n]->objectUnpacked.connect(boost::bind(&Telemetry::objectUnpacked, this, _1));
		}
		if ((eventMask & EV_UPDATED) != 0) {
			objs[n]->objectUpdatedAuto.connect(boost::bind(&Telemetry::objectUpdatedAuto, this, _1));
		}
		if ((eventMask & EV_UPDATED_MANUAL) != 0) {
			objs[n]->objectUpdatedManual.connect(boost::bind(&Telemetry::objectUpdatedManual, this, _1));
		}
		if ((eventMask & EV_UPDATED_PERIODIC) != 0) {
			objs[n]->objectUpdatedPeriodic.connect(boost::bind(&Telemetry::objectUpdatedPeriodic, this, _1));
		}
		if ((eventMask & EV_UPDATE_REQ) != 0) {
			objs[n]->updateRequested.connect(boost::bind(&Telemetry::updateRequested, this, _1));
		}
	}
}

/** Update an object based on its metadata properties
 */
void Telemetry::updateObject(UAVObject *obj, uint32_t eventType)
{
	// Get metadata
	UAVObject::Metadata metadata     = obj->getMetadata();
	UAVObject::UpdateMode updateMode = UAVObject::GetGcsTelemetryUpdateMode(metadata);

	// Setup object depending on update mode
	int32_t eventMask;

	if (updateMode == UAVObject::UPDATEMODE_PERIODIC) {
		// Set update period
		setUpdatePeriod(obj, metadata.gcsTelemetryUpdatePeriod);
		// Connect signals for all instances
		eventMask = EV_UPDATED_MANUAL | EV_UPDATE_REQ | EV_UPDATED_PERIODIC;
		if (dynamic_cast<UAVMetaObject *>(obj) != NULL) {
			eventMask |= EV_UNPACKED; // we also need to act on remote updates (unpack events)
		}
		connectToObjectInstances(obj, eventMask);

	} else if (updateMode == UAVObject::UPDATEMODE_ONCHANGE) {
		// Set update period
		setUpdatePeriod(obj, 0);
		// Connect signals for all instances
		eventMask = EV_UPDATED | EV_UPDATED_MANUAL | EV_UPDATE_REQ;
		if (dynamic_cast<UAVMetaObject *>(obj) != NULL) {
			eventMask |= EV_UNPACKED; // we also need to act on remote updates (unpack events)
		}
		connectToObjectInstances(obj, eventMask);

	} else if (updateMode == UAVObject::UPDATEMODE_THROTTLED) {
		// If we received a periodic update, we can change back to update on change
		if ((eventType == EV_UPDATED_PERIODIC) || (eventType == EV_NONE)) {
			// Set update period
			if (eventType == EV_NONE) {
				setUpdatePeriod(obj, metadata.gcsTelemetryUpdatePeriod);
			}
			// Connect signals for all instances
			eventMask = EV_UPDATED | EV_UPDATED_MANUAL | EV_UPDATE_REQ | EV_UPDATED_PERIODIC;

		} else {
			// Otherwise, we just received an object update,
			// so switch to periodic for the timeout period to prevent more updates
			// Connect signals for all instances
			eventMask = EV_UPDATED | EV_UPDATED_MANUAL | EV_UPDATE_REQ;
		}
		if (dynamic_cast<UAVMetaObject *>(obj) != NULL) {
			eventMask |= EV_UNPACKED; // we also need to act on remote updates (unpack events)
		}
		connectToObjectInstances(obj, eventMask);

	} else if (updateMode == UAVObject::UPDATEMODE_MANUAL) {
		// Set update period
		setUpdatePeriod(obj, 0);
		// Connect signals for all instances
		eventMask = EV_UPDATED_MANUAL | EV_UPDATE_REQ;
		if (dynamic_cast<UAVMetaObject *>(obj) != NULL) {
			eventMask |= EV_UNPACKED; // we also need to act on remote updates (unpack events)
		}
		connectToObjectInstances(obj, eventMask);
	}
}

/** Called when a transaction is successfully completed (uavtalk event)
 */
void Telemetry::transactionCompleted(UAVObject *obj, bool success)
{
	// Lookup the transaction in the transaction map.
	uint32_t objId = obj->getObjID();

	std::map<uint32_t, ObjectTransactionInfo *>::iterator itr = transMap.find(objId);
	if (itr != transMap.end()) {
		ObjectTransactionInfo *transInfo = itr->second;
		// Remove this transaction as it's complete.
		transInfo->timer.cancel();
		transMap.erase(itr);
		delete transInfo;
		// Send signal
		obj->transactionCompleted(obj, success);
		// Process new object updates from queue
		processObjectQueue();
	} else {
		ROS_DEBUG_NAMED("Telemetry", "Error: received a transaction completed when did not expect it.");
	}
}

/** Called when a transaction is not completed within the timeout period (timer event)
 */
void Telemetry::transactionTimeout(boost::system::error_code error, ObjectTransactionInfo *transInfo)
{
	if (error)
		return;

	// Check if more retries are pending
	if (transInfo->retriesRemaining > 0) {
		--transInfo->retriesRemaining;
		processObjectTransaction(transInfo);
		++txRetries;

	} else {
		// Terminate transaction
		utalk->cancelTransaction(transInfo->obj);
		// Send signal
		transInfo->obj->transactionCompleted(transInfo->obj, false);
		// Remove this transaction as it's complete.
		transMap.erase(transInfo->obj->getObjID());
		delete transInfo;
		// Process new object updates from queue
		processObjectQueue();
		++txErrors;
	}
}

/** Start an object transaction with UAVTalk, all information is stored in transInfo
 */
void Telemetry::processObjectTransaction(ObjectTransactionInfo *transInfo)
{
	// Initiate transaction
	if (transInfo->objRequest) {
		utalk->sendObjectRequest(transInfo->obj, transInfo->allInstances);
	} else {
		utalk->sendObject(transInfo->obj, transInfo->acked, transInfo->allInstances);
	}

	// Start timer if a response is expected
	if (transInfo->objRequest || transInfo->acked) {
		transInfo->timer.expires_from_now(boost::posix_time::milliseconds(REQ_TIMEOUT_MS));
		transInfo->timer.async_wait(boost::bind(&Telemetry::transactionTimeout, this,
					boost::asio::placeholders::error, transInfo));
	} else {
		// Otherwise, remove this transaction as it's complete.
		transMap.erase(transInfo->obj->getObjID());
		delete transInfo;
	}
}

/** Process the event received from an object
 */
void Telemetry::processObjectUpdates(UAVObject *obj, EventMask event, bool allInstances, bool priority)
{
	// Push event into queue
	ObjectQueueInfo objInfo;

	objInfo.obj   = obj;
	objInfo.event = event;
	objInfo.allInstances = allInstances;

	if (priority) {
		if (objPriorityQueue.size() < MAX_QUEUE_SIZE) {
			objPriorityQueue.push(objInfo);
		} else {
			++txErrors;
			obj->transactionCompleted(obj, false); // emit
			ROS_WARN_STREAM_NAMED("Telemetry", "Telemetry: priority event queue is full, event lost (" << obj->getName() << ")");
		}

	} else {
		if (objQueue.size() < MAX_QUEUE_SIZE) {
			objQueue.push(objInfo);
		} else {
			++txErrors;
			obj->transactionCompleted(obj, false); // emit
		}
	}

	// Process the transaction
	processObjectQueue();
}

/** Process events from the object queue
 */
void Telemetry::processObjectQueue()
{
	// Get object information from queue (first the priority and then the regular queue)
	ObjectQueueInfo objInfo;

	if (!objPriorityQueue.empty()) {
		objInfo = objPriorityQueue.front();
		objPriorityQueue.pop();
	} else if (!objQueue.empty()) {
		objInfo = objQueue.front();
		objQueue.pop();
	} else {
		return;
	}

	// Check if a connection has been established, only process GCSTelemetryStats updates
	// (used to establish the connection)
	GCSTelemetryStats::DataFields gcsStats = gcsStatsObj->getData();
	if (gcsStats.Status != GCSTelemetryStats::STATUS_CONNECTED) {
		// clear queue
		while (!objQueue.empty())
			objQueue.pop();

		if (objInfo.obj->getObjID() != GCSTelemetryStats::OBJID
				&& objInfo.obj->getObjID() != OPLinkSettings::OBJID
				&& objInfo.obj->getObjID() != ObjectPersistence::OBJID) {
			objInfo.obj->transactionCompleted(objInfo.obj, false);
			return;
		}
	}

	// Setup transaction (skip if unpack event)
	UAVObject::Metadata metadata     = objInfo.obj->getMetadata();
	UAVObject::UpdateMode updateMode = UAVObject::GetGcsTelemetryUpdateMode(metadata);
	if ((objInfo.event != EV_UNPACKED) &&
			((objInfo.event != EV_UPDATED_PERIODIC) || (updateMode != UAVObject::UPDATEMODE_THROTTLED))) {

		std::map<uint32_t, ObjectTransactionInfo *>::iterator itr = transMap.find(objInfo.obj->getObjID());
		if (itr != transMap.end()) {
			ROS_DEBUG_STREAM_NAMED("Telemetry", "!!!!!! Making request for an object: " << objInfo.obj->getName() << " for which a request is already in progress!!!!!!");
		}

		UAVObject::Metadata metadata     = objInfo.obj->getMetadata();
		ObjectTransactionInfo *transInfo = new ObjectTransactionInfo(io_service);
		transInfo->obj                   = objInfo.obj;
		transInfo->allInstances          = objInfo.allInstances;
		transInfo->retriesRemaining      = MAX_RETRIES;
		transInfo->acked                 = UAVObject::GetGcsTelemetryAcked(metadata);

		if (objInfo.event == EV_UPDATED || objInfo.event == EV_UPDATED_MANUAL || objInfo.event == EV_UPDATED_PERIODIC) {
			transInfo->objRequest = false;
		} else if (objInfo.event == EV_UPDATE_REQ) {
			transInfo->objRequest = true;
		}

		// Insert the transaction into the transaction map.
		transMap[objInfo.obj->getObjID()] = transInfo;
		processObjectTransaction(transInfo);
	}

	// If this is a metaobject then make necessary telemetry updates
	UAVMetaObject *metaobj = dynamic_cast<UAVMetaObject *>(objInfo.obj);
	if (metaobj != NULL) {
		updateObject(metaobj->getParentObject(), EV_NONE);
	} else if (updateMode != UAVObject::UPDATEMODE_THROTTLED) {
		updateObject(objInfo.obj, objInfo.event);
	}

	// The fact we received an unpacked event does not mean that
	// we do not have additional objects still in the queue,
	// so we have to reschedule queue processing to make sure they are not
	// stuck:
	if (objInfo.event == EV_UNPACKED) {
		processObjectQueue();
	}
}

/** Check is any objects are pending for periodic updates
 * TODO: Clean-up
 */
void Telemetry::processPeriodicUpdates(boost::system::error_code error)
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	if (error)
		return;

	// Iterate through each object and update its timer, if zero then transmit object.
	// Also calculate smallest delay to next update (will be used for setting timeToNextUpdateMs)
	int32_t minDelay  = MAX_UPDATE_PERIOD_MS;
	ObjectTimeInfo *objinfo;
	int32_t offset;
	boost::posix_time::ptime start_time, end_time;
	boost::posix_time::time_duration elapsed;

	for (int n = 0; n < objList.size(); ++n) {
		objinfo = &objList[n];
		// If object is configured for periodic updates
		if (objinfo->updatePeriodMs > 0) {
			objinfo->timeToNextUpdateMs -= timeToNextUpdateMs;
			// Check if time for the next update
			if (objinfo->timeToNextUpdateMs <= 0) {
				// Reset timer
				offset = (-objinfo->timeToNextUpdateMs) % objinfo->updatePeriodMs;
				objinfo->timeToNextUpdateMs = objinfo->updatePeriodMs - offset;
				// Send object
				start_time = boost::posix_time::microsec_clock::universal_time();
				processObjectUpdates(objinfo->obj, EV_UPDATED_PERIODIC, true, false);
				end_time = boost::posix_time::microsec_clock::universal_time();
				elapsed = end_time - start_time;
				// Update timeToNextUpdateMs with the elapsed delay of sending the object;
				timeToNextUpdateMs += elapsed.total_milliseconds();
			}
			// Update minimum delay
			if (objinfo->timeToNextUpdateMs < minDelay) {
				minDelay = objinfo->timeToNextUpdateMs;
			}
		}
	}

	// Check if delay for the next update is too short
	if (minDelay < MIN_UPDATE_PERIOD_MS) {
		minDelay = MIN_UPDATE_PERIOD_MS;
	}

	// Done
	timeToNextUpdateMs = minDelay;

	// Restart timer
	updateTimer.expires_from_now(boost::posix_time::milliseconds(timeToNextUpdateMs));
	updateTimer.async_wait(boost::bind(&Telemetry::processPeriodicUpdates, this, boost::asio::placeholders::error));
}

Telemetry::TelemetryStats Telemetry::getStats()
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	// Get UAVTalk stats
	UAVTalk::ComStats utalkStats = utalk->getStats();

	// Update stats
	TelemetryStats stats;

	stats.txBytes       = utalkStats.txBytes;
	stats.rxBytes       = utalkStats.rxBytes;
	stats.txObjectBytes = utalkStats.txObjectBytes;
	stats.rxObjectBytes = utalkStats.rxObjectBytes;
	stats.rxObjects     = utalkStats.rxObjects;
	stats.txObjects     = utalkStats.txObjects;
	stats.txErrors      = utalkStats.txErrors + txErrors;
	stats.rxErrors      = utalkStats.rxErrors;
	stats.txRetries     = txRetries;

	// Done
	return stats;
}

void Telemetry::resetStats()
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	utalk->resetStats();
	txErrors  = 0;
	txRetries = 0;
}

void Telemetry::objectUpdatedAuto(UAVObject *obj)
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	processObjectUpdates(obj, EV_UPDATED, false, true);
}

void Telemetry::objectUpdatedManual(UAVObject *obj)
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	processObjectUpdates(obj, EV_UPDATED_MANUAL, false, true);
}

void Telemetry::objectUpdatedPeriodic(UAVObject *obj)
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	processObjectUpdates(obj, EV_UPDATED_PERIODIC, false, true);
}

void Telemetry::objectUnpacked(UAVObject *obj)
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	processObjectUpdates(obj, EV_UNPACKED, false, true);
}

void Telemetry::updateRequested(UAVObject *obj)
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	processObjectUpdates(obj, EV_UPDATE_REQ, false, true);
}

void Telemetry::newObject(UAVObject *obj)
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	registerObject(obj);
}

void Telemetry::newInstance(UAVObject *obj)
{
	boost::recursive_mutex::scoped_lock lock(mutex);

	registerObject(obj);
}

ObjectTransactionInfo::ObjectTransactionInfo(boost::asio::io_service &io) :
	timer(io)
{
	obj = 0;
	allInstances     = false;
	objRequest       = false;
	retriesRemaining = 0;
	acked = false;
}

