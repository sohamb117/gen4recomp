/*
 * The frame channel's Windows primitives, with no <windows.h> in sight,
 * the same quarantine pc_win_fiber.h explains: pc/src/pc_view.c includes the
 * guest SDK's headers, and windows.h cannot be read through the guest's
 * preprocessor. pc/src/pc_win_ipc.c is the one translation unit behind this
 * surface, and every HANDLE crosses it as a void pointer.
 *
 * What the shapes mirror. POSIX gave the channel two lives: an anonymous
 * unlinked object whose descriptor a child inherits (--view), and a named
 * object a stranger attaches to (--view-shm). Windows has both natively,
 * an inheritable anonymous file mapping, and a named one in the session's
 * Local\ namespace whose object dies with its last handle, which is the
 * unlink POSIX had to do by hand. Empty on POSIX; nothing there calls it.
 */

#ifndef POKEDIAMOND_PC_WIN_IPC_H
#define POKEDIAMOND_PC_WIN_IPC_H

/*
 * Create a shared object of `size` bytes and map it read-write.
 * `name` NULL makes it anonymous and the handle inheritable (the --view
 * shape); otherwise it becomes Local\<name> (the --view-shm shape).
 * Returns the view, with *handle_out the mapping handle, pass it to
 * pcw_ipc_handle_value() for a child's command line, and to
 * pcw_shm_close() at the end. NULL on failure with pcw_ipc_error() set.
 */
void *pcw_shm_create(const char *name, unsigned size, void **handle_out);

/* Unmap a view and close its mapping handle. Either may be NULL. */
void pcw_shm_close(void *view, void *handle);

/* The number a child turns back into an inherited HANDLE. */
unsigned long pcw_ipc_handle_value(void *handle);

/*
 * Start `argv` (NULL-terminated, argv[0] the executable) with inheritable
 * handles passed through. Returns a process handle for pcw_process_gone(),
 * or NULL with pcw_ipc_error() set. The command line is quoted here, once,
 * because Windows gives a child one string and every program re-splits it.
 */
void *pcw_spawn(char *const argv[]);

/* 1 once that process has exited. Never blocks; closes the handle when it
 * answers 1, so ask through one owner. */
int pcw_process_gone(void *proc);

/* An inheritable handle to *this* process, for a child that wants to notice
 * the port dying without a word, the Windows spelling of getppid() == 1. */
void *pcw_self_process(void);

/*
 * This process's id, for the frame channel's publisher stamp. It goes
 * through this surface rather than being called directly because a call
 * with no prototype gets the cdecl symbol and the import library exports
 * the stdcall one; which is a link error if you are lucky.
 */
unsigned pcw_self_pid(void);

/* 1 if that process id still belongs to a live process. Used to notice a
 * viewer that went away without saying so; a pid nobody owns answers 0. */
int pcw_pid_alive(unsigned pid);

/* The directory this executable runs from, slash-terminated. Returns 0 if
 * it cannot be told. The Windows spelling of readlink(/proc/self/exe). */
void *pcw_valloc_fixed(void *want, unsigned len);
int pcw_module_dir(char *out, unsigned cap);

/* The last failure, as text. */
const char *pcw_ipc_error(void);

#endif /* POKEDIAMOND_PC_WIN_IPC_H */
