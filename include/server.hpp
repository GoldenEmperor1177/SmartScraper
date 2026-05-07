#pragma once

// Daemonise and start the HTTP API server.
// In the parent process this returns immediately after fork.
// The daemon child loops forever until SIGTERM.
void run_server_daemon();

// Run the server in the foreground (no fork). Used by systemd ExecStart.
// Blocks until SIGTERM/SIGINT.
void run_server_foreground();
