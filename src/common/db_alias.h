/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Dmitry Yemanov
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2002 Dmitry Yemanov <dimitr@users.sf.net>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#ifndef JRD_DB_ALIAS_H
#define JRD_DB_ALIAS_H

#include "../common/classes/fb_string.h"
#include "../common/classes/RefCounted.h"

class Config;

bool resolveAlias(const Firebird::PathName& alias,
				  Firebird::PathName& file,
				  Firebird::RefPtr<const Config>* config);

bool expandDatabaseName(Firebird::PathName alias,
						Firebird::PathName& file,
						Firebird::RefPtr<const Config>* config);

bool notifyDatabaseName(const Firebird::PathName& file);

#endif // JRD_DB_ALIAS_H
