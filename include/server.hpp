#pragma once

// Daemonise and start the HTTP API server.
// In the parent process this returns immediately after fork.
// The daemon child loops forever until SIGTERM.
void run_server_daemon();
