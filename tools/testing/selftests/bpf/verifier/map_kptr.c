/* Common tests */
{
	"map_kptr: BPF_ST imm != 0",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ST_MEM(BPF_DW, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "BPF_ST imm must be 0 when storing to kptr at off=0",
},
{
	"map_kptr: size != bpf_size_to_bytes(BPF_DW)",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ST_MEM(BPF_W, BPF_REG_0, 0, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "kptr access size must be BPF_DW",
},
{
	"map_kptr: map_value non-const var_off",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_MOV64_REG(BPF_REG_3, BPF_REG_0),
	BPF_LDX_MEM(BPF_DW, BPF_REG_2, BPF_REG_0, 0),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_2, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_2, 0),
	BPF_JMP_IMM(BPF_JLE, BPF_REG_2, 4, 1),
	BPF_EXIT_INSN(),
	BPF_JMP_IMM(BPF_JGE, BPF_REG_2, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ALU64_REG(BPF_ADD, BPF_REG_3, BPF_REG_2),
	BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_3, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "kptr access cannot have variable offset",
	.flags = F_NEEDS_EFFICIENT_UNALIGNED_ACCESS,
},
{
	"map_kptr: bpf_kptr_xchg non-const var_off",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_MOV64_REG(BPF_REG_3, BPF_REG_0),
	BPF_LDX_MEM(BPF_DW, BPF_REG_2, BPF_REG_0, 0),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_2, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_2, 0),
	BPF_JMP_IMM(BPF_JLE, BPF_REG_2, 4, 1),
	BPF_EXIT_INSN(),
	BPF_JMP_IMM(BPF_JGE, BPF_REG_2, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ALU64_REG(BPF_ADD, BPF_REG_3, BPF_REG_2),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_3),
	BPF_MOV64_IMM(BPF_REG_2, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_kptr_xchg),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "R1 doesn't have constant offset. kptr has to be at the constant offset",
},
{
	"map_kptr: unaligned boundary load/store",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 7),
	BPF_ST_MEM(BPF_DW, BPF_REG_0, 0, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "kptr access misaligned expected=0 off=7",
	.flags = F_NEEDS_EFFICIENT_UNALIGNED_ACCESS,
},
{
	"map_kptr: reject var_off != 0",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_DW, BPF_REG_1, BPF_REG_0, 0),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_1, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_W, BPF_REG_2, BPF_REG_1, 0),
	BPF_JMP_IMM(BPF_JLE, BPF_REG_2, 4, 1),
	BPF_EXIT_INSN(),
	BPF_JMP_IMM(BPF_JGE, BPF_REG_2, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ALU64_REG(BPF_ADD, BPF_REG_1, BPF_REG_2),
	BPF_STX_MEM(BPF_DW, BPF_REG_0, BPF_REG_1, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "variable untrusted_ptr_ access var_off=(0x0; 0x7) disallowed",
},
/* Tests for unreferenced PTR_TO_BTF_ID */
{
	"map_kptr: unref: reject btf_struct_ids_match == false",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_DW, BPF_REG_1, BPF_REG_0, 0),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_1, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, 4),
	BPF_STX_MEM(BPF_DW, BPF_REG_0, BPF_REG_1, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "invalid kptr access, R1 type=untrusted_ptr_prog_test_ref_kfunc expected=ptr_prog_test",
},
{
	"map_kptr: unref: loaded pointer marked as untrusted",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_0, 0),
	BPF_LDX_MEM(BPF_W, BPF_REG_0, BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "R0 invalid mem access 'untrusted_ptr_or_null_'",
},
{
	"map_kptr: unref: correct in kernel type size",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_0, 0),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_0, 32),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "access beyond struct prog_test_ref_kfunc at off 32 size 8",
},
{
	"map_kptr: unref: inherit PTR_UNTRUSTED on struct walk",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_0, 0),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_DW, BPF_REG_1, BPF_REG_0, 16),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_this_cpu_ptr),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "R1 type=untrusted_ptr_ expected=percpu_ptr_",
},
{
	"map_kptr: unref: no reference state created",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_LDX_MEM(BPF_DW, BPF_REG_0, BPF_REG_0, 0),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = ACCEPT,
},
{
	"map_kptr: unref: bpf_kptr_xchg rejected",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_0),
	BPF_MOV64_IMM(BPF_REG_2, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_kptr_xchg),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "off=0 kptr isn't referenced kptr",
},
/* Tests for referenced PTR_TO_BTF_ID */
{
	"map_kptr: ref: loaded pointer marked as untrusted",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_MOV64_IMM(BPF_REG_1, 0),
	BPF_LDX_MEM(BPF_DW, BPF_REG_1, BPF_REG_0, 8),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_this_cpu_ptr),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "R1 type=rcu_ptr_or_null_ expected=percpu_ptr_",
},
{
	"map_kptr: ref: reject off != 0",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_MOV64_REG(BPF_REG_7, BPF_REG_0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_0),
	BPF_MOV64_IMM(BPF_REG_2, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_kptr_xchg),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_7),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, 8),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_kptr_xchg),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "invalid kptr access, R2 type=ptr_prog_test_ref_kfunc expected=ptr_prog_test_member",
},
{
	"map_kptr: ref: reference state created and released on xchg",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 8),
	BPF_MOV64_REG(BPF_REG_7, BPF_REG_0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_10),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_1, -8),
	BPF_ST_MEM(BPF_DW, BPF_REG_1, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, BPF_PSEUDO_KFUNC_CALL, 0, 0),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_7),
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_kptr_xchg),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "Unreleased reference id=5 alloc_insn=20",
	.fixup_kfunc_btf_id = {
		{ "bpf_kfunc_call_test_acquire", 15 },
	}
},
{
	"map_kptr: ref: reject STX",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_MOV64_REG(BPF_REG_1, 0),
	BPF_STX_MEM(BPF_DW, BPF_REG_0, BPF_REG_1, 8),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "store to referenced kptr disallowed",
},
{
	"map_kptr: ref: reject ST",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_ST_MEM(BPF_DW, BPF_REG_0, 8, 0),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "store to referenced kptr disallowed",
},
{
	"map_kptr: reject helper access to kptr",
	.insns = {
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),
	BPF_LD_MAP_FD(BPF_REG_6, 0),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),
	BPF_MOV64_IMM(BPF_REG_0, 0),
	BPF_ST_MEM(BPF_W, BPF_REG_2, 0, 0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem),
	BPF_JMP_IMM(BPF_JNE, BPF_REG_0, 0, 1),
	BPF_EXIT_INSN(),
	BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
	BPF_ALU64_IMM(BPF_ADD, BPF_REG_0, 2),
	BPF_MOV64_REG(BPF_REG_2, BPF_REG_0),
	BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_delete_elem),
	BPF_EXIT_INSN(),
	},
	.prog_type = BPF_PROG_TYPE_SCHED_CLS,
	.fixup_map_kptr = { 1 },
	.result = REJECT,
	.errstr = "kptr cannot be accessed indirectly by helper",
},
