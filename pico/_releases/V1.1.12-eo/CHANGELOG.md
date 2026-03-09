# V1.1.12-eo

## TFTP session cleanup after cancel/error

- **Session cleanup:** When a TFTP upload or download is cancelled or ends in error, the job is now cleaned up so the next TFTP run starts fresh and does not "reconnect" to the previous session.
- **TFTPResetSessionState():** New function in `tftpstate.c` resets session state (taskid, blockTransferred, tsize, retries, error, status to IDLE). It does not clear unitNum, dir, hostname, filename (those are set by the Control Panel before each run).
- **When it runs:** At the start of every `ExecuteTFTP()` (on Core 0), before creating the TFTP task. So when you start a new TFTP transfer after a failed or cancelled one, any stale state from the previous run is cleared and the new session uses only the new parameters.
- **Resource cleanup:** The previous behaviour (delete task on exit so destructor runs, tearing down UDP PCB and CYW43) is unchanged; the new reset ensures `tftp_state` is also in a clean state for the next run.
