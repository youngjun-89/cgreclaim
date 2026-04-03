#include "cgr_log.h"
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	fprintf(stderr, "=== Test: Lazy Log Open ===\n\n");

	fprintf(stderr, "[Test 1] Log without prior cgr_log_open()\n");
	cgr_log_file(CGR_LOG_INFO, "First message without eager open (lazy should trigger)");

	fprintf(stderr, "[Test 2] Log again (should use already-open file)\n");
	cgr_log_file(CGR_LOG_WARN, "Second message after lazy open succeeded");

	fprintf(stderr, "[Test 3] Explicit cgr_log_open() after first write (should be no-op)\n");
	if (cgr_log_open() == 0)
		fprintf(stderr, "  cgr_log_open() returned 0 (already open)\n");
	else
		fprintf(stderr, "  cgr_log_open() returned -1 (unexpected)\n");

	cgr_log_file(CGR_LOG_DEBUG, "Third message after explicit open");

	fprintf(stderr, "[Test 4] Check log file exists and has content\n");
	fprintf(stderr, "  Log path: %s\n", CGR_LOG_PATH);

	cgr_log_close();
	fprintf(stderr, "\n[Done] Closed log file\n");

	return 0;
}
