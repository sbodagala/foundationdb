/*
 * BulkDumpCommand.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2024 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstddef>
#include <fmt/core.h>
#include "fdbcli/fdbcli.actor.h"
#include "fdbclient/BulkDumping.h"
#include "fdbclient/BulkLoading.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "flow/Arena.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

namespace fdb_cli {

ACTOR Future<bool> getOngoingBulkDumpJob(Database cx) {
	state Transaction tr(cx);
	loop {
		try {
			Optional<BulkDumpState> job = wait(getSubmittedBulkDumpJob(&tr));
			if (job.present()) {
				fmt::println("Running bulk dumping job: {}", job.get().getJobId().toString());
				return true;
			} else {
				fmt::println("No bulk dumping job is running");
				return false;
			}
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<Void> getBulkDumpCompleteRanges(Database cx, KeyRange rangeToRead) {
	try {
		size_t finishCount = wait(getBulkDumpCompleteTaskCount(cx, rangeToRead));
		fmt::println("Finished {} tasks", finishCount);
	} catch (Error& e) {
		if (e.code() == error_code_timed_out) {
			fmt::println("timed out");
		}
	}
	return Void();
}

ACTOR Future<UID> bulkDumpCommandActor(Database cx, std::vector<StringRef> tokens) {
	state BulkDumpState bulkDumpJob;
	if (tokencmp(tokens[1], "mode")) {
		if (tokens.size() != 2 && tokens.size() != 3) {
			printLongDesc(tokens[0]);
			return UID();
		}
		if (tokens.size() == 2) {
			int old = wait(getBulkDumpMode(cx));
			if (old == 0) {
				fmt::println("Bulk dump is disabled");
			} else if (old == 1) {
				fmt::println("Bulk dump is enabled");
			} else {
				fmt::println("Invalid mode value {}", old);
			}
			return UID();
		}
		ASSERT(tokens.size() == 3);
		if (tokencmp(tokens[2], "on")) {
			int old = wait(setBulkDumpMode(cx, 1));
			TraceEvent("SetBulkDumpModeCommand").detail("OldValue", old).detail("NewValue", 1);
			return UID();
		} else if (tokencmp(tokens[2], "off")) {
			int old = wait(setBulkDumpMode(cx, 0));
			TraceEvent("SetBulkDumpModeCommand").detail("OldValue", old).detail("NewValue", 0);
			return UID();
		} else {
			printLongDesc(tokens[0]);
			return UID();
		}

	} else if (tokencmp(tokens[1], "local")) {
		if (tokens.size() != 5) {
			printLongDesc(tokens[0]);
			return UID();
		}
		Key rangeBegin = tokens[2];
		Key rangeEnd = tokens[3];
		// Bulk load can only inject data to normal key space, aka "" ~ \xff
		if (rangeBegin >= rangeEnd || rangeEnd > normalKeys.end) {
			printLongDesc(tokens[0]);
			return UID();
		}
		std::string jobRoot = tokens[4].toString();
		KeyRange range = Standalone(KeyRangeRef(rangeBegin, rangeEnd));
		bulkDumpJob = createBulkDumpJob(range, jobRoot, BulkLoadType::SST, BulkLoadTransportMethod::CP);
		wait(submitBulkDumpJob(cx, bulkDumpJob));
		return bulkDumpJob.getJobId();

	} else if (tokencmp(tokens[1], "blobstore")) {
		if (tokens.size() != 5) {
			printLongDesc(tokens[0]);
			return UID();
		}
		Key rangeBegin = tokens[2];
		Key rangeEnd = tokens[3];
		// Bulk load can only inject data to normal key space, aka "" ~ \xff
		if (rangeBegin >= rangeEnd || rangeEnd > normalKeys.end) {
			printLongDesc(tokens[0]);
			return UID();
		}
		std::string jobRoot = tokens[4].toString();
		KeyRange range = Standalone(KeyRangeRef(rangeBegin, rangeEnd));
		bulkDumpJob = createBulkDumpJob(range, jobRoot, BulkLoadType::SST, BulkLoadTransportMethod::BLOBSTORE);
		wait(submitBulkDumpJob(cx, bulkDumpJob));
		return bulkDumpJob.getJobId();

	} else if (tokencmp(tokens[1], "cancel")) {
		if (tokens.size() != 3) {
			printLongDesc(tokens[0]);
			return UID();
		}
		state UID jobId = UID::fromString(tokens[2].toString());
		wait(cancelBulkDumpJob(cx, jobId));
		fmt::println("Job {} has been cancelled. No new tasks will be spawned.", jobId.toString());
		return UID();

	} else if (tokencmp(tokens[1], "status")) {
		if (tokens.size() != 4) {
			printLongDesc(tokens[0]);
			return UID();
		}
		bool anyJob = wait(getOngoingBulkDumpJob(cx));
		if (!anyJob) {
			return UID();
		}
		Key rangeBegin = tokens[2];
		Key rangeEnd = tokens[3];
		if (rangeBegin >= rangeEnd || rangeEnd > normalKeys.end) {
			printLongDesc(tokens[0]);
			return UID();
		}
		KeyRange range = Standalone(KeyRangeRef(rangeBegin, rangeEnd));
		wait(getBulkDumpCompleteRanges(cx, range));
		return UID();

	} else {
		printUsage(tokens[0]);
		return UID();
	}
}

CommandFactory bulkDumpFactory(
    "bulkdump",
    CommandHelp("bulkdump [mode|local|blobstore|status|cancel] [ARGs]",
                "bulkdump commands",
                "To set bulkdump mode: `bulkdump mode [on|off]'\n"
                "To dump a range to a local dir: `bulkdump local <BEGINKEY> <ENDKEY> <DIR>`\n"
                " where <DIR> is the local directory to write SST files and <BEGINKEY>\n"
                " to <ENDKEY> denotes the key/value range to dump.\n"
                "To dump a range to s3: `bulkdump blobstore <JOBID> <BEGINKEY> <ENDKEY> <URL>`\n"
                " where <URL> is the 'bloblstore' url of the s3 bucket to write the SST files\n"
                " to -- see https://apple.github.io/foundationdb/backups.html#backup-urls --\n"
                " and <BEGINKEY> to <ENDKEY> denotes the keyvalue range to dump.\n"
                "To cancel current bulkdump job: `bulkdump cancel <JOBID>`\n"
                "To get completed bulkdump ranges: `bulkdump status <BEGINKEY> <ENDKEY>`\n"));
} // namespace fdb_cli
