/*
 *	Variables and functions used by the code in sleep.c
 */

extern char wakeup_code_start, wakeup_code_end;

extern unsigned long saved_video_mode;
extern long saved_magic;
extern unsigned long init_rsp;
extern void (*initial_code)(void);

extern int wakeup_pmode_return;
extern char swsusp_pg_dir[PAGE_SIZE];

extern unsigned long acpi_copy_wakeup_routine(unsigned long);
extern unsigned long setup_trampoline(void);
extern void wakeup_long64(void);
