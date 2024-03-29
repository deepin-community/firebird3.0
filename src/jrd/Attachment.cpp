/*
 *      PROGRAM:        JRD access method
 *      MODULE:         Attachment.cpp
 *      DESCRIPTION:    JRD Attachment class
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 *
 */

#include "firebird.h"
#include "../jrd/Attachment.h"
#include "../jrd/Database.h"
#include "../jrd/Function.h"
#include "../jrd/nbak.h"
#include "../jrd/trace/TraceManager.h"
#include "../jrd/PreparedStatement.h"
#include "../jrd/tra.h"
#include "../jrd/intl.h"

#include "../jrd/blb_proto.h"
#include "../jrd/exe_proto.h"
#include "../jrd/ext_proto.h"
#include "../jrd/intl_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/tra_proto.h"

#include "../jrd/extds/ExtDS.h"

#include "../common/classes/fb_string.h"
#include "../common/classes/MetaName.h"
#include "../common/StatusArg.h"
#include "../common/isc_proto.h"


using namespace Jrd;
using namespace Firebird;


// static method
Jrd::Attachment* Jrd::Attachment::create(Database* dbb, JProvider* provider)
{
	MemoryPool* const pool = dbb->createPool();

	try
	{
		Attachment* const attachment = FB_NEW_POOL(*pool) Attachment(pool, dbb, provider);
		pool->setStatsGroup(attachment->att_memory_stats);
		return attachment;
	}
	catch (const Firebird::Exception&)
	{
		dbb->deletePool(pool);
		throw;
	}
}


// static method
void Jrd::Attachment::destroy(Attachment* const attachment)
{
	if (!attachment)
		return;

	StableAttachmentPart* sAtt = attachment->getStable();
	fb_assert(sAtt);
	if (sAtt)
	{
		// break link between attachment and its stable part
		sAtt->cancel();
		attachment->setStable(NULL);

		sAtt->manualUnlock(attachment->att_flags);
	}

	thread_db* tdbb = JRD_get_thread_data();

	jrd_tra* sysTransaction = attachment->getSysTransaction();
	if (sysTransaction)
	{
		// unwind any active system requests
		while (sysTransaction->tra_requests)
			EXE_unwind(tdbb, sysTransaction->tra_requests);

		jrd_tra::destroy(NULL, sysTransaction);
	}

	Database* const dbb = attachment->att_database;
	MemoryPool* const pool = attachment->att_pool;
	Firebird::MemoryStats temp_stats;
	pool->setStatsGroup(temp_stats);

	delete attachment;

	dbb->deletePool(pool);
}


MemoryPool* Jrd::Attachment::createPool()
{
	MemoryPool* const pool = MemoryPool::createPool(att_pool, att_memory_stats);
	att_pools.add(pool);
	return pool;
}


void Jrd::Attachment::deletePool(MemoryPool* pool)
{
	if (pool)
	{
		FB_SIZE_T pos;
		if (att_pools.find(pool, pos))
			att_pools.remove(pos);

		MemoryPool::deletePool(pool);
	}
}


bool Jrd::Attachment::backupStateWriteLock(thread_db* tdbb, SSHORT wait)
{
	if (att_backup_state_counter++)
		return true;

	if (att_database->dbb_backup_manager->lockStateWrite(tdbb, wait))
		return true;

	att_backup_state_counter--;
	return false;
}


void Jrd::Attachment::backupStateWriteUnLock(thread_db* tdbb)
{
	if (--att_backup_state_counter == 0)
		att_database->dbb_backup_manager->unlockStateWrite(tdbb);
}


bool Jrd::Attachment::backupStateReadLock(thread_db* tdbb, SSHORT wait)
{
	if (att_backup_state_counter++)
		return true;

	if (att_database->dbb_backup_manager->lockStateRead(tdbb, wait))
		return true;

	att_backup_state_counter--;
	return false;
}


void Jrd::Attachment::backupStateReadUnLock(thread_db* tdbb)
{
	if (--att_backup_state_counter == 0)
		att_database->dbb_backup_manager->unlockStateRead(tdbb);
}


Jrd::Attachment::Attachment(MemoryPool* pool, Database* dbb, JProvider* provider)
	: att_pool(pool),
	  att_memory_stats(&dbb->dbb_memory_stats),
	  att_database(dbb),
	  att_user_ids(*pool),
	  att_requests(*pool),
	  att_lock_owner_id(Database::getLockOwnerId()),
	  att_backup_state_counter(0),
	  att_stats(*pool),
	  att_base_stats(*pool),
	  att_working_directory(*pool),
	  att_filename(*pool),
	  att_timestamp(Firebird::TimeStamp::getCurrentTimeStamp()),
	  att_context_vars(*pool),
	  ddlTriggersContext(*pool),
	  att_network_protocol(*pool),
	  att_remote_address(*pool),
	  att_remote_process(*pool),
	  att_client_version(*pool),
	  att_remote_protocol(*pool),
	  att_remote_host(*pool),
	  att_remote_os_user(*pool),
	  att_dsql_cache(*pool),
	  att_udf_pointers(*pool),
	  att_ext_connection(NULL),
	  att_ext_call_depth(0),
	  att_trace_manager(FB_NEW_POOL(*att_pool) TraceManager(this)),
	  att_utility(UTIL_NONE),
	  att_procedures(*pool),
	  att_functions(*pool),
	  att_internal(*pool),
	  att_dyn_req(*pool),
	  att_charsets(*pool),
	  att_charset_ids(*pool),
	  att_pools(*pool),
	  att_provider(provider)
{
	att_internal.grow(irq_MAX);
	att_dyn_req.grow(drq_MAX);
}


Jrd::Attachment::~Attachment()
{
	delete att_trace_manager;

	for (Function** iter = att_functions.begin(); iter < att_functions.end(); ++iter)
	{
		Function* const function = *iter;
		if (function)
			delete function;
	}

	for (jrd_prc** iter = att_procedures.begin(); iter < att_procedures.end(); ++iter)
	{
		jrd_prc* const procedure = *iter;
		if (procedure)
			delete procedure;
	}

	while (att_pools.hasData())
		deletePool(att_pools.pop());

	// For normal attachments that happens in release_attachment(),
	// but for special ones like GC should be done also in dtor -
	// they do not (and should not) call release_attachment().
	// It's no danger calling detachLocks()
	// once more here because it nulls att_long_locks.
	//		AP 2007
	detachLocks();
}


Jrd::PreparedStatement* Jrd::Attachment::prepareStatement(thread_db* tdbb, jrd_tra* transaction,
	const string& text, Firebird::MemoryPool* pool)
{
	pool = pool ? pool : tdbb->getDefaultPool();
	return FB_NEW_POOL(*pool) PreparedStatement(tdbb, *pool, this, transaction, text, true);
}


Jrd::PreparedStatement* Jrd::Attachment::prepareStatement(thread_db* tdbb, jrd_tra* transaction,
	const PreparedStatement::Builder& builder, Firebird::MemoryPool* pool)
{
	pool = pool ? pool : tdbb->getDefaultPool();
	return FB_NEW_POOL(*pool) PreparedStatement(tdbb, *pool, this, transaction, builder, true);
}


PreparedStatement* Jrd::Attachment::prepareUserStatement(thread_db* tdbb, jrd_tra* transaction,
	const string& text, Firebird::MemoryPool* pool)
{
	pool = pool ? pool : tdbb->getDefaultPool();
	return FB_NEW_POOL(*pool) PreparedStatement(tdbb, *pool, this, transaction, text, false);
}


MetaName Jrd::Attachment::nameToMetaCharSet(thread_db* tdbb, const MetaName& name)
{
	if (att_charset == CS_METADATA || att_charset == CS_NONE)
		return name;

	UCHAR buffer[MAX_SQL_IDENTIFIER_LEN * 4 + 1];
	ULONG len = INTL_convert_bytes(tdbb, CS_METADATA, buffer, sizeof(buffer) - 1,
		att_charset, (const BYTE*) name.c_str(), name.length(), ERR_post);

	if (len > MAX_SQL_IDENTIFIER_LEN)
	{
		(Arg::Gds(isc_dsql_error) <<
			Arg::Gds(isc_sqlerr) << Arg::Num(-104) <<
			Arg::Gds(isc_dyn_name_longer)
		).raise();
	}

	buffer[len] = '\0';

	return MetaName((const char*) buffer);
}


MetaName Jrd::Attachment::nameToUserCharSet(thread_db* tdbb, const MetaName& name)
{
	if (att_charset == CS_METADATA || att_charset == CS_NONE)
		return name;

	UCHAR buffer[MAX_SQL_IDENTIFIER_SIZE];
	ULONG len = INTL_convert_bytes(tdbb, att_charset, buffer, MAX_SQL_IDENTIFIER_LEN,
		CS_METADATA, (const BYTE*) name.c_str(), name.length(), ERR_post);
	buffer[len] = '\0';

	return MetaName((const char*) buffer);
}


string Jrd::Attachment::stringToMetaCharSet(thread_db* tdbb, const string& str,
	const char* charSet)
{
	USHORT charSetId = att_charset;

	if (charSet)
	{
		if (!MET_get_char_coll_subtype(tdbb, &charSetId, (const UCHAR*) charSet,
				static_cast<USHORT>(strlen(charSet))))
		{
			(Arg::Gds(isc_charset_not_found) << Arg::Str(charSet)).raise();
		}
	}

	if (charSetId == CS_METADATA || charSetId == CS_NONE)
		return str;

	HalfStaticArray<UCHAR, BUFFER_MEDIUM> buffer(str.length() * sizeof(ULONG));
	ULONG len = INTL_convert_bytes(tdbb, CS_METADATA, buffer.begin(), buffer.getCapacity(),
		charSetId, (const BYTE*) str.c_str(), str.length(), ERR_post);

	return string((char*) buffer.begin(), len);
}


string Jrd::Attachment::stringToUserCharSet(thread_db* tdbb, const string& str)
{
	if (att_charset == CS_METADATA || att_charset == CS_NONE)
		return str;

	HalfStaticArray<UCHAR, BUFFER_MEDIUM> buffer(str.length() * sizeof(ULONG));
	ULONG len = INTL_convert_bytes(tdbb, att_charset, buffer.begin(), buffer.getCapacity(),
		CS_METADATA, (const BYTE*) str.c_str(), str.length(), ERR_post);

	return string((char*) buffer.begin(), len);
}


// We store in CS_METADATA.
void Jrd::Attachment::storeMetaDataBlob(thread_db* tdbb, jrd_tra* transaction,
	bid* blobId, const string& text, USHORT fromCharSet)
{
	UCharBuffer bpb;
	if (fromCharSet != CS_METADATA)
		BLB_gen_bpb(isc_blob_text, isc_blob_text, fromCharSet, CS_METADATA, bpb);

	blb* blob = blb::create2(tdbb, transaction, blobId, bpb.getCount(), bpb.begin());
	try
	{
		blob->BLB_put_data(tdbb, (const UCHAR*) text.c_str(), text.length());
	}
	catch (const Exception&)
	{
		blob->BLB_close(tdbb);
		throw;
	}

	blob->BLB_close(tdbb);
}


// We store raw stuff; don't attempt to translate.
void Jrd::Attachment::storeBinaryBlob(thread_db* tdbb, jrd_tra* transaction,
	bid* blobId, const ByteChunk& chunk)
{
	blb* blob = blb::create2(tdbb, transaction, blobId, 0, NULL);
	try
	{
		blob->BLB_put_data(tdbb, chunk.data, chunk.length);
	}
	catch (const Exception&)
	{
		blob->BLB_close(tdbb);
		throw;
	}

	blob->BLB_close(tdbb);
}


void Jrd::Attachment::signalCancel()
{
	att_flags |= ATT_cancel_raise;

	if (att_ext_connection && att_ext_connection->isConnected())
		att_ext_connection->cancelExecution(false);

	LCK_cancel_wait(this);
}


void Jrd::Attachment::signalShutdown()
{
	att_flags |= ATT_shutdown;

	if (att_ext_connection && att_ext_connection->isConnected())
		att_ext_connection->cancelExecution(true);

	LCK_cancel_wait(this);
}


void Jrd::Attachment::mergeStats()
{
	MutexLockGuard guard(att_database->dbb_stats_mutex, FB_FUNCTION);
	att_database->dbb_stats.adjust(att_base_stats, att_stats, true);
	att_base_stats.assign(att_stats);
}


// Find an inactive incarnation of a system request. If necessary, clone it.
jrd_req* Jrd::Attachment::findSystemRequest(thread_db* tdbb, USHORT id, USHORT which)
{
	static const int MAX_RECURSION = 100;

	// If the request hasn't been compiled or isn't active, there're nothing to do.

	//Database::CheckoutLockGuard guard(this, dbb_cmp_clone_mutex);

	fb_assert(which == IRQ_REQUESTS || which == DYN_REQUESTS);

	JrdStatement* statement = (which == IRQ_REQUESTS ? att_internal[id] : att_dyn_req[id]);

	if (!statement)
		return NULL;

	// Look for requests until we find one that is available.

	for (int n = 0;; ++n)
	{
		if (n > MAX_RECURSION)
		{
			ERR_post(Arg::Gds(isc_no_meta_update) <<
						Arg::Gds(isc_req_depth_exceeded) << Arg::Num(MAX_RECURSION));
			// Msg363 "request depth exceeded. (Recursive definition?)"
		}

		jrd_req* clone = statement->getRequest(tdbb, n);

		if (!(clone->req_flags & (req_active | req_reserved)))
		{
			clone->req_flags |= req_reserved;
			return clone;
		}
	}
}

void Jrd::Attachment::initLocks(thread_db* tdbb)
{
	// Take out lock on attachment id

	const lock_ast_t ast = (att_flags & ATT_system) ? NULL : blockingAstShutdown;

	Lock* lock = FB_NEW_RPT(*att_pool, 0)
		Lock(tdbb, sizeof(AttNumber), LCK_attachment, this, ast);
	att_id_lock = lock;
	lock->lck_key.lck_long = att_attachment_id;
	LCK_lock(tdbb, lock, LCK_EX, LCK_WAIT);

	// Allocate and take the monitoring lock

	lock = FB_NEW_RPT(*att_pool, 0)
		Lock(tdbb, sizeof(AttNumber), LCK_monitor, this, blockingAstMonitor);
	att_monitor_lock = lock;
	lock->lck_key.lck_long = att_attachment_id;
	LCK_lock(tdbb, lock, LCK_EX, LCK_WAIT);

	// Unless we're a system attachment, allocate the cancellation lock

	if (!(att_flags & ATT_system))
	{
		lock = FB_NEW_RPT(*att_pool, 0)
			Lock(tdbb, sizeof(AttNumber), LCK_cancel, this, blockingAstCancel);
		att_cancel_lock = lock;
		lock->lck_key.lck_long = att_attachment_id;
	}
}

void Jrd::Attachment::releaseLocks(thread_db* tdbb)
{
	// Go through relations and indices and release
	// all existence locks that might have been taken.

	vec<jrd_rel*>* rvector = att_relations;

	if (rvector)
	{
		vec<jrd_rel*>::iterator ptr, end;

		for (ptr = rvector->begin(), end = rvector->end(); ptr < end; ++ptr)
		{
			jrd_rel* relation = *ptr;

			if (relation)
			{
				if (relation->rel_existence_lock)
				{
					LCK_release(tdbb, relation->rel_existence_lock);
					relation->rel_flags |= REL_check_existence;
					relation->rel_use_count = 0;
				}

				if (relation->rel_partners_lock)
				{
					LCK_release(tdbb, relation->rel_partners_lock);
					relation->rel_flags |= REL_check_partners;
				}

				if (relation->rel_rescan_lock)
				{
					LCK_release(tdbb, relation->rel_rescan_lock);
					relation->rel_flags &= ~REL_scanned;
				}

				if (relation->rel_gc_lock)
				{
					LCK_release(tdbb, relation->rel_gc_lock);
					relation->rel_flags |= REL_gc_lockneed;
				}

				for (IndexLock* index = relation->rel_index_locks; index; index = index->idl_next)
				{
					if (index->idl_lock)
					{
						index->idl_count = 0;
						LCK_release(tdbb, index->idl_lock);
					}
				}

				for (IndexBlock* index = relation->rel_index_blocks; index; index = index->idb_next)
				{
					if (index->idb_lock)
						LCK_release(tdbb, index->idb_lock);
				}
			}
		}
	}

	// Release all procedure existence locks that might have been taken

	for (jrd_prc** iter = att_procedures.begin(); iter < att_procedures.end(); ++iter)
	{
		jrd_prc* const procedure = *iter;

		if (procedure)
		{
			if (procedure->existenceLock)
			{
				LCK_release(tdbb, procedure->existenceLock);
				procedure->flags |= Routine::FLAG_CHECK_EXISTENCE;
				procedure->useCount = 0;
			}
		}
	}

	// Release all function existence locks that might have been taken

	for (Function** iter = att_functions.begin(); iter < att_functions.end(); ++iter)
	{
		Function* const function = *iter;

		if (function)
			function->releaseLocks(tdbb);
	}

	// Release collation existence locks

	releaseIntlObjects(tdbb);

	// Release the DSQL cache locks

	DSqlCache::Accessor accessor(&att_dsql_cache);
	for (bool getResult = accessor.getFirst(); getResult; getResult = accessor.getNext())
		LCK_release(tdbb, accessor.current()->second.lock);

	// Release the remaining locks

	if (att_id_lock)
		LCK_release(tdbb, att_id_lock);

	if (att_cancel_lock)
		LCK_release(tdbb, att_cancel_lock);

	if (att_monitor_lock)
		LCK_release(tdbb, att_monitor_lock);

	if (att_temp_pg_lock)
		LCK_release(tdbb, att_temp_pg_lock);

	// And release the system requests

	for (JrdStatement** itr = att_internal.begin(); itr != att_internal.end(); ++itr)
	{
		if (*itr)
			(*itr)->release(tdbb);
	}

	for (JrdStatement** itr = att_dyn_req.begin(); itr != att_dyn_req.end(); ++itr)
	{
		if (*itr)
			(*itr)->release(tdbb);
	}
}

void Jrd::Attachment::detachLocks()
{
/**************************************
 *
 *	d e t a c h L o c k s
 *
 **************************************
 *
 * Functional description
 * Bug #7781, need to null out the attachment pointer of all locks which
 * were hung off this attachment block, to ensure that the attachment
 * block doesn't get dereferenced after it is released
 *
 **************************************/
	if (!att_long_locks)
		return;

	Lock* long_lock = att_long_locks;
	while (long_lock)
		long_lock = long_lock->detach();

	att_long_locks = NULL;
}

void Jrd::Attachment::releaseRelations(thread_db* tdbb)
{
	if (att_relations)
	{
		vec<jrd_rel*>* vector = att_relations;

		for (vec<jrd_rel*>::iterator ptr = vector->begin(), end = vector->end(); ptr < end; ++ptr)
		{
			jrd_rel* relation = *ptr;

			if (relation)
			{
				if (relation->rel_file)
					EXT_fini(relation, false);

				delete relation;
			}
		}
	}
}

int Jrd::Attachment::blockingAstShutdown(void* ast_object)
{
	Jrd::Attachment* const attachment = static_cast<Jrd::Attachment*>(ast_object);

	try
	{
		Database* const dbb = attachment->att_database;

		AsyncContextHolder tdbb(dbb, FB_FUNCTION, attachment->att_id_lock);

		attachment->signalShutdown();

		JRD_shutdown_attachment(attachment);
	}
	catch (const Exception&)
	{} // no-op

	return 0;
}

int Jrd::Attachment::blockingAstCancel(void* ast_object)
{
	Jrd::Attachment* const attachment = static_cast<Jrd::Attachment*>(ast_object);

	try
	{
		Database* const dbb = attachment->att_database;

		AsyncContextHolder tdbb(dbb, FB_FUNCTION, attachment->att_cancel_lock);

		attachment->signalCancel();

		LCK_release(tdbb, attachment->att_cancel_lock);
	}
	catch (const Exception&)
	{} // no-op

	return 0;
}

int Jrd::Attachment::blockingAstMonitor(void* ast_object)
{
	Jrd::Attachment* const attachment = static_cast<Jrd::Attachment*>(ast_object);

	try
	{
		Database* const dbb = attachment->att_database;

		AsyncContextHolder tdbb(dbb, FB_FUNCTION, attachment->att_monitor_lock);

		if (!(attachment->att_flags & ATT_monitor_done))
		{
			try
			{
				Monitoring::dumpAttachment(tdbb, attachment);
			}
			catch (const Exception& ex)
			{
				iscLogException("Cannot dump the monitoring data", ex);
			}

			LCK_downgrade(tdbb, attachment->att_monitor_lock);
			attachment->att_flags |= ATT_monitor_done;
		}
	}
	catch (const Exception&)
	{} // no-op

	return 0;
}

void Jrd::Attachment::SyncGuard::init(const char* f, bool
#ifdef DEV_BUILD
	optional
#endif
	)
{
	fb_assert(optional || jStable);

	if (jStable)
	{
		jStable->getSync()->enter(f);
		if (!jStable->getHandle())
		{
			jStable->getSync()->leave();
			Arg::Gds(isc_att_shutdown).raise();
		}
	}
}

void AttachmentsRefHolder::debugHelper(const char* from)
{
	RefDeb(DEB_RLS_JATT, from);
}

void StableAttachmentPart::manualLock(ULONG& flags)
{
	fb_assert(!(flags & ATT_manual_lock));
	async.enter(FB_FUNCTION);
	mainSync.enter(FB_FUNCTION);
	flags |= (ATT_manual_lock | ATT_async_manual_lock);
}

void StableAttachmentPart::manualUnlock(ULONG& flags)
{
	if (flags & ATT_manual_lock)
	{
		flags &= ~ATT_manual_lock;
		mainSync.leave();
	}
	manualAsyncUnlock(flags);
}

void StableAttachmentPart::manualAsyncUnlock(ULONG& flags)
{
	if (flags & ATT_async_manual_lock)
	{
		flags &= ~ATT_async_manual_lock;
		async.leave();
	}
}

JAttachment* Attachment::getInterface() throw()
{
	return att_stable->getInterface();
}

void Attachment::setInterface(JAttachment* iface) throw()
{
	att_stable->setInterface(iface);
}

UserId* Attachment::getUserId(const string& userName)
{
	// It's necessary to keep specified sql role of user
	if (att_user && att_user->usr_user_name == userName)
		return att_user;

	UserId* result = NULL;
	if (!att_user_ids.get(userName, result))
	{
		result = FB_NEW_POOL(*att_pool) UserId(*att_pool);
		result->usr_user_name = userName;
		att_user_ids.put(userName, result);
	}
	return result;
}
