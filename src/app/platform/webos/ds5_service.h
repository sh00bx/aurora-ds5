#pragma once

/*
 * Kick off the bundled DS5 raw-ACL transport: heal the service's elevation if an
 * app update knocked it out, then start it. Returns immediately — the work runs
 * on a detached thread and the app never depends on its outcome.
 */
void ds5_service_bootstrap(void);
