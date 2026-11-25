libsmb2 v6.0.0 from https://github.com/sahlberg/libsmb2 (tag v6.0.0), the `lib/`
and `include/` trees as released (minus other platforms' makefiles) with the two
local changes below, each marked "Mosaico:" in the source. LGPL-2.1:
see COPYING and LICENCE-LGPL-2.1.txt. Used by the Files app to browse SMB shares.

One local change: `lib/sync.c` `wait_for_reply` gives up after 3x the context
timeout even while the socket is open (the sync calls could otherwise wait
forever for a reply that never comes, which froze the Files app).
Second local change: a failed `smb2_opendir` freed the sync wrapper's
`cb_data` along with the dir (`free_smb2dir`) while `wait_for_reply` and
`smb2_opendir` still read it, so the call could return a garbage dir and
`smb2_closedir` crashed. The error paths in `lib/libsmb2.c` now leave
`cb_data` to the caller, and `smb2_opendir` frees it when it fails.
