// Branch and scalar-loop words for the GL2 back end's real loops.
//
// Until now oops-gl had no backward branch at all: every `for` was unrolled, and a loop the
// unroller could not finish was refused at compile time. That is safe but it is not GLSL. A real
// loop needs three things this file pins down - an unconditional branch, a branch taken when the
// exec mask has gone empty, and the scalar compare/decrement that drives the trip guard.
//
// The offsets matter as much as the opcodes. `simm16` is a *signed word count relative to the
// instruction after the branch*, so a branch to itself is -1, not 0, and the sign lives in a
// 16-bit field that has to be masked before it goes in the word. Both directions are assembled
// here so the encoder's forward and backward paths each have a witness.
//
//   clang -target amdgcn-amd-amdhsa -mcpu=gfx1030 -c tools/shader/branch.s -o /tmp/b.o
//   objdump -s -j .text /tmp/b.o
//
// Wave32 throughout: the mask register is `exec_lo`, never `exec`.

.text
.globl branch_words
branch_words:

// --- the three branches ------------------------------------------------------------------
// A backward branch to the label immediately above the branch: the shape a loop's tail takes.
loop_top:
	s_nop 0
	s_branch loop_top

// The loop's exit test. `s_cbranch_execz` is what makes a narrowed loop cheap: when every lane
// has left, the body is skipped rather than executed with a zero mask.
	s_cbranch_execz loop_exit

// The trip guard's branch, on the scalar condition code rather than the exec mask.
	s_cbranch_scc1 loop_exit

// A forward branch, for the positive-offset path of the fixup.
	s_branch loop_exit
	s_nop 0
	s_nop 0
loop_exit:

// --- the trip guard's arithmetic ---------------------------------------------------------
// A counter in an SGPR, incremented once per trip and compared against a ceiling. This is the
// part that makes a real loop safe to emit: a condition that never goes false still terminates,
// because the guard fires regardless of what the lanes are doing. A shader that hangs does not
// draw a wrong frame - it takes the GPU with it.
	s_mov_b32 s20, 0
	s_add_u32 s20, s20, 1
	s_cmp_ge_u32 s20, 0x100
	s_cmp_lg_u32 s20, 0

// Both sides of the inline-constant boundary. The scalar inline table runs 0..64 in operands
// 128..192, so a ceiling of 64 is one word and a ceiling of 65 is two - and an encoder that
// took the inline path for 65 would compare against operand 193, which is *-4.0*.
	s_cmp_ge_u32 s20, 64
	s_cmp_ge_u32 s20, 65

// --- mask bookkeeping across a loop ------------------------------------------------------
// The set of lanes still going round. `break` clears a lane out of this, the top of the loop
// loads it into exec, and the exit restores the mask the loop was entered with.
	s_mov_b32 s21, exec_lo
	s_and_b32 s21, s21, vcc_lo
	s_andn2_b32 s21, s21, exec_lo
	s_mov_b32 exec_lo, s21

	s_endpgm
