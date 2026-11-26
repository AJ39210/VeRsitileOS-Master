BITS 64
DEFAULT REL

SECTION .rodata.user_payload

GLOBAL user_payload_start
GLOBAL user_payload_end

%macro VERIFY_READABLE_FILE 1-2
    mov eax, 6
    lea rdi, [rel %1]
    int 0x80
    test rax, rax
    js .proc_checks_failed

    mov r12, rax
    mov eax, 2
    mov rdi, r12
    mov rsi, rsp
%if %0 == 2
    mov edx, %2
%else
    mov edx, 16
%endif
    int 0x80
    test rax, rax
    jle %%close_failed

    mov eax, 7
    mov rdi, r12
    int 0x80
    jmp %%done

%%close_failed:
    mov eax, 7
    mov rdi, r12
    int 0x80
    jmp .proc_checks_failed
%%done:
%endmacro

user_payload_start:
    mov eax, 1
    mov edi, 1
    lea rsi, [rel parent_message]
    mov edx, parent_message_end - parent_message
    int 0x80

    mov eax, 8
    lea rdi, [rel user_worker]
    int 0x80
    test rax, rax
    js .terminal_start
    mov r12, rax

.wait_for_worker:
    mov eax, 21
    mov rdi, r12
    int 0x80
    test rax, rax
    jz .terminal_start
    mov eax, 11
    int 0x80
    jmp .wait_for_worker

.terminal_start:
    mov eax, 12
    lea rdi, [rel terminal_path]
    int 0x80
    test rax, rax
    js terminal_spawn_failed

    mov eax, 11
    int 0x80

userspace_idle:
    mov eax, 11
    int 0x80
    jmp userspace_idle

user_worker:
    sub rsp, 128
    xor r13, r13

    mov eax, 1
    mov edi, 1
    lea rsi, [rel worker_message]
    mov edx, worker_message_end - worker_message
    int 0x80

    VERIFY_READABLE_FILE proc_cpuinfo_path
    VERIFY_READABLE_FILE proc_meminfo_path
    VERIFY_READABLE_FILE proc_devices_path
    VERIFY_READABLE_FILE proc_partitions_path, 64
    VERIFY_READABLE_FILE proc_pid1_status_path
    VERIFY_READABLE_FILE proc_pid2_status_path
    VERIFY_READABLE_FILE sys_osrelease_path
    VERIFY_READABLE_FILE dev_zero_path
    VERIFY_READABLE_FILE dev_sr0_path

    mov eax, 1
    mov edi, 1
    lea rsi, [rel mounts_read_message]
    mov edx, mounts_read_message_end - mounts_read_message
    int 0x80

    mov eax, 12
    lea rdi, [rel elf_test_path]
    int 0x80
    test rax, rax
    js .elf_spawn_failed
    mov r13, rax

    mov eax, 6
    lea rdi, [rel dev_console_path]
    int 0x80
    test rax, rax
    js .proc_checks_failed
    mov r12, rax
    mov eax, 1
    mov rdi, r12
    lea rsi, [rel dev_console_message]
    mov edx, dev_console_message_end - dev_console_message
    int 0x80
    mov eax, 7
    mov rdi, r12
    int 0x80

    mov eax, 4
    mov edi, 4096
    int 0x80
    test rax, rax
    jz .read_input

    mov r12, rax
    mov rax, 0x5652534f53595343
    mov [r12], rax

    mov eax, 5
    mov rdi, r12
    int 0x80

.read_input:
    mov eax, 4
    mov edi, 16
    int 0x80
    test rax, rax
    jz .open_file

    mov r12, rax
    mov eax, 2
    xor edi, edi
    mov rsi, r12
    mov edx, 16
    int 0x80

    mov eax, 5
    mov rdi, r12
    int 0x80

.open_file:
    mov eax, 6
    lea rdi, [rel file_path]
    int 0x80
    test rax, rax
    js .exit

    mov r12, rax
    mov eax, 2
    mov rdi, r12
    mov rsi, rsp
    mov edx, 16
    int 0x80
    test rax, rax
    jle .close_file

    mov eax, 1
    mov edi, 1
    lea rsi, [rel file_read_message]
    mov edx, file_read_message_end - file_read_message
    int 0x80

.close_file:
    mov rdi, r12
    mov eax, 7
    int 0x80

.exit:
.wait_for_elf_test:
    test r13, r13
    jz .preemptible_work_start
    mov eax, 21
    mov rdi, r13
    int 0x80
    test rax, rax
    jz .preemptible_work_start
    mov eax, 11
    int 0x80
    jmp .wait_for_elf_test

.preemptible_work_start:
    mov ecx, 10000000
.preemptible_work:
    pause
    loop .preemptible_work

    mov eax, 9
    xor edi, edi
    int 0x80
    jmp $

.proc_checks_failed:
    mov eax, 1
    mov edi, 1
    lea rsi, [rel mounts_failed_message]
    mov edx, mounts_failed_message_end - mounts_failed_message
    int 0x80
    jmp .exit

.elf_spawn_failed:
    mov eax, 1
    mov edi, 1
    lea rsi, [rel elf_spawn_error_message]
    mov edx, elf_spawn_error_message_end - elf_spawn_error_message
    int 0x80
    jmp .exit

terminal_spawn_failed:
    mov eax, 1
    mov edi, 1
    lea rsi, [rel terminal_spawn_error_message]
    mov edx, terminal_spawn_error_message_end - terminal_spawn_error_message
    int 0x80

    mov eax, 9
    int 0x80
    jmp userspace_idle

parent_message:
    db "user task entered ring 3", 10
parent_message_end:

worker_message:
    db "spawned task is running", 10
worker_message_end:

file_path:
    db "/BOOT/KERNEL.ELF", 0
file_read_message:
    db "filesystem read syscall succeeded", 10
file_read_message_end:

mounts_read_message:
    db "procfs/devfs/sysfs reads succeeded", 10
mounts_read_message_end:

mounts_failed_message:
    db "procfs/devfs/sysfs check failed", 10
mounts_failed_message_end:

dev_console_message:
    db "devfs console write succeeded", 10
dev_console_message_end:

proc_cpuinfo_path:
    db "/proc/cpuinfo", 0
proc_meminfo_path:
    db "/proc/meminfo", 0
proc_devices_path:
    db "/proc/devices", 0
proc_partitions_path:
    db "/proc/partitions", 0
proc_pid1_status_path:
    db "/proc/1/status", 0
proc_pid2_status_path:
    db "/proc/2/status", 0
sys_osrelease_path:
    db "/sys/kernel/osrelease", 0
dev_zero_path:
    db "/dev/zero", 0
dev_sr0_path:
    db "/dev/sr0", 0
dev_console_path:
    db "/dev/console", 0
elf_test_path:
    db "/bin/elf_test", 0
terminal_path:
    db "/bin/terminal", 0
elf_spawn_error_message:
    db "C ELF spawn failed", 10
elf_spawn_error_message_end:
terminal_spawn_error_message:
    db "Terminal could not start; see serial ELF diagnostics", 10
terminal_spawn_error_message_end:

user_payload_end:

SECTION .note.GNU-stack noalloc noexec nowrite progbits
