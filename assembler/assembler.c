#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "symbol_table.h"
#include "hack_standard.h"
#include "asm_malloc.h"
#include "exit.h"


#define MAX_LINE_LEN  200
#define MAX_LABEL_LEN 198
#define INIT_MEMORY_ALLOC 400

#define OPCODE_STR "%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c"

#define OPCODE_TO_BINARY(opcode) \
  (opcode & 0x8000 ? '1' : '0'), \
  (opcode & 0x4000 ? '1' : '0'), \
  (opcode & 0x2000 ? '1' : '0'), \
  (opcode & 0x1000 ? '1' : '0'), \
  (opcode & 0x0800 ? '1' : '0'), \
  (opcode & 0x0400 ? '1' : '0'), \
  (opcode & 0x0200 ? '1' : '0'), \
  (opcode & 0x0100 ? '1' : '0'), \
  (opcode & 0x0080 ? '1' : '0'), \
  (opcode & 0x0040 ? '1' : '0'), \
  (opcode & 0x0020 ? '1' : '0'), \
  (opcode & 0x0010 ? '1' : '0'), \
  (opcode & 0x0008 ? '1' : '0'), \
  (opcode & 0x0004 ? '1' : '0'), \
  (opcode & 0x0002 ? '1' : '0'), \
  (opcode & 0x0001 ? '1' : '0')

#define INST_TO_OPCODE(inst, opcode)   \
    do {                               \
        (opcode) |= (7 << 13);         \
        (opcode) |= (inst).a << 12;    \
        (opcode) |= (inst).comp << 6;  \
        (opcode) |= (inst).dest << 3;  \
        (opcode) |= (inst).jump;       \
    } while (0)


typedef enum inst_id {
    INST_INVALID = -1,
    INST_A,
    INST_C,
} inst_id;

/*
 * The opcode of an A-instruction is a 0 bit, followed by a 15-bit address.
 * Sometimes we are just provided with a symbol's name instead of a direct
 * address, and we use this symbol to resolve the real address later.
 *
 * When resolved is true that means that we already have a numerical address.
 * When false, the symbol field holds a string that represents a symbol's name.
 */
typedef struct a_inst {
    union {
        char *symbol;
        hack_addr address;
    } operand;
    bool resolved;
} a_inst;

/*
 * The opcode of a C-instruction is the following:
 * 1 1 1 a c1 c2 c3 c4 c5 c6 d1 d2 d3 j1 j2 j3
 */
typedef struct c_inst {
    int16_t a:1;
    int16_t comp:7; // make these bigger than their opcode length in order to
    int16_t dest:4; // be able to hold invalid values as well
    int16_t jump:4;
} c_inst ;

/*
 * A generic instruction that can either hold an A-instruction or a C-instruction.
 * In order to determine which type of instruction it holds we have to check
 * the id field.
 */
typedef struct generic_inst {
    union {
        c_inst c;
        a_inst a;
    } inst;
    inst_id id;
} generic_inst;



/*
 * Check whether the given path corresponds to a regular file and if that's
 * the case try to open the file using fopen.
 */
FILE *file_open_or_bail(const char *filename, const char *mode)
{
    struct stat path_stat;

    if (stat(filename, &path_stat) != 0) {
        exit_program(EXIT_FILE_DOES_NOT_EXIST, filename);
    }
    if(!S_ISREG(path_stat.st_mode)) {
        exit_program(EXIT_NOT_REGULAR_FILE, filename);
    }

    FILE *fp = fopen(filename, mode);

    if (fp == NULL) {
        exit_program(EXIT_CANNOT_OPEN_FILE, filename);
    }

    return fp;
}

/*
 * Strip a line from comments and remove *all* whitespace.
 * Comments are indicated by two adjacent / characters.
 *
 * E.g. "  A  = M  // comment " becomes "A=M"
 */
char *strip_comments_and_whitespace(char *s) {
    // sanity checks
    if (s == NULL) {
        return NULL;
    } else if (!*s) {
        return s;
    }

    char s_new[strlen(s) + 1];
    int i = 0;

    for (char *s2 = s; *s2; s2++) {
        if (*s2 == '/' && *(s2+1) == '/') {
            break; // a comment starts from this point on; skip rest of the line
        } else if (!isspace(*s2)) {
            s_new[i++] = *s2;
        }
    }
    s_new[i] = '\0';

    strcpy(s, s_new); // this is safe because s_new is smaller than s

    return s;
}

/**
 * Parse a line and check whether it defines a label. If so, store label's
 * name in the label paremeter and return true. Else return false.
 * If the return value is false, the contents of the label argument shall not
 * be accessed.
 */
bool is_label(const char *line, char *label)
{
    const char *s = line;
    const char *s_end = s + strlen(line) - 1;

    if (*s != '(') {
        return false;
    }
    if (*s_end != ')') {
        return false;
    }

    for (s=s+1; s < s_end; s++) {
        if (*s == '(' || *s == ')') {
            return false;
        }
        *label++ = *s;
    }
    *label = '\0';

    return true;
}

/**
 * Populate some predefined symbols in the symbol table.
 */
void populate_predefined_symbols(SymbolTable table)
{
    for (int i = 0; i < NUM_PREDEFINED_SYMS; i++) {
        struct predef_symbol s = predef_symbols[i];
        symtab_add(table, s.name, s.address);
    }
}

/*
 * Parse an A-instruction and determine if it is valid or not. Store the
 * operand (the after @ part) either as string or as a hack address in the
 * instruction. Invalid instructions are those that the operand begins with
 * digit(s) but contains other characters as well. All other instructions are
 * valid. Return true when instruction is valid, else false.
 *
 * Examples:
 * Return true for "@5439", "@LOOP", "@SYMBOL123", "@L99P"
 * Return false for "@1ABC", "@1234LOOP"
 */
bool parse_A_instruction(const char *line, a_inst *inst)
{
    // +1 in malloc not needed because we don't store 1st char
    inst->operand.symbol = asm_malloc(strlen(line));
    char *s = inst->operand.symbol;
    strcpy(s, line+1);

    char *endptr = NULL;
    hack_addr result = strtol(s, &endptr, 10);

    if (s == endptr) {
        // operand is a symbol
        inst->resolved = false;
        return true;
    } else if (errno == 0 && *endptr != 0) {
        // operand is invalid; begins with digit(s) but continues with other chars
        free(inst->operand.symbol);
        return false;
    } else {
        // operand is a number
        free(inst->operand.symbol);
        inst->operand.address = result;
        inst->resolved = true;
    }

    return true;
}

/*
 * Parse a C-instruction and split it into its three parts; dest, comp and jmp.
 * Then do the mapping between strings and HACK instruction codes.
 */
void parse_C_instruction(char *line, c_inst *inst)
{
    char *tmp = NULL;
    char *dest = NULL;
    char *comp = NULL;
    char *jmp = NULL;
    int a;

    tmp = strtok(line, ";");
    jmp = strtok(NULL, "");
    dest = strtok(tmp, "=");
    comp = strtok(NULL, "");

    if (comp == NULL) {
        comp = dest;
        dest = NULL;
    }

    inst->dest = str_to_destid(dest);
    inst->comp = str_to_compid(comp, &a);
    inst->a = 1 ? a : 0;
    inst->jump = str_to_jumpid(jmp);
}


int main(int argc, const char *argv[])
{
    if (argc != 2) {
        exit_program(EXIT_MANY_FILES);
    }

    /*
     * Indicates number of current instruction.
     * This counts only real instructions, not empty lines, comments nor labels.
     */
    unsigned instruction_num = 0;
    /*
     * Indicates current file line that is being processed.
     */
    unsigned line_num = 0;
    /*
     * Holds current instruction.
     */
    generic_inst inst;
    /*
     * A buffer keeping all valid instructions after reading them from the file.
     */
    generic_inst *instructions = NULL;
    /*
     * This indicates for how many instructions we have allocated memory.
     * Initially we allocate INIT_MEMORY_ALLOC memory and later, whenever we
     * hit this limit and need to store more instructions in memory we double
     * this value.
     */
    unsigned allocated_mem = 0;
    /*
     * Holds current line read.
     */
    char line[MAX_LINE_LEN + 1];
    char tmp_line[MAX_LINE_LEN + 1];
    /*
     * Used to temporarily store the label of the current instruction, if
     * the current instruction declares a new label.
     */
    char label[MAX_LABEL_LEN + 1];

    FILE *fp = file_open_or_bail(argv[1], "r");

    SymbolTable symtab = symtab_init();
    populate_predefined_symbols(symtab);

    /* First pass */

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        if (instruction_num > MAX_INSTRUCTION) {
            exit_program(EXIT_TOO_MANY_INSTRUCTIONS, MAX_INSTRUCTION + 1);
        }

        strip_comments_and_whitespace(line);

        if (!*line) {
            continue; // skip empty lines
        }

        if (is_label(line, label)) {
            if (!isalpha(*label)) {
                exit_program(EXIT_INVALID_LABEL, line_num, line);
            }
            if (symtab_lookup(symtab, label) != SYMBOL_NOT_FOUND) {
                exit_program(EXIT_SYMBOL_ALREADY_EXISTS, line_num, line);
            }
            symtab_add(symtab, label, instruction_num);
            continue;
        }

        if (*line == '@') {
            if (!parse_A_instruction(line, &inst.inst.a)) {
                exit_program(EXIT_INVALID_A_INST, line_num, line);
            }
            inst.id = INST_A;
        } else {
            strcpy(tmp_line, line); // safe because they have same lengths
            parse_C_instruction(tmp_line, &inst.inst.c);

            if (inst.inst.c.dest == DEST_INVALID) {
                exit_program(EXIT_INVALID_C_DEST, line_num, line);
            } else if (inst.inst.c.comp == COMP_INVALID) {
                exit_program(EXIT_INVALID_C_COMP, line_num, line);
            } else if (inst.inst.c.jump == JMP_INVALID) {
                exit_program(EXIT_INVALID_C_JUMP, line_num, line);
            }

            inst.id = INST_C;
        }

        if (instruction_num == allocated_mem) {
            // We need more memory for storing instructions.
            // Double what we already have or make our first allocation of default value.
            unsigned tmp = allocated_mem ? allocated_mem * 2 : INIT_MEMORY_ALLOC;
            allocated_mem = tmp > MAX_INSTRUCTION ? MAX_INSTRUCTION : tmp;
            instructions = asm_realloc(instructions, allocated_mem * sizeof(generic_inst));
        }

        instructions[instruction_num++] = inst;
    }

    fclose(fp);

    // Reduce allocated memory to the amount of memory that we really need for
    // storing all instructions read.
    instructions = asm_realloc(instructions, instruction_num * sizeof(generic_inst));

    /* Second pass */

    opcode op;

    for (unsigned i = 0; i < instruction_num; i++) {
        op = 0;
        inst = instructions[i];

        if (inst.id == INST_A) {
            if (! inst.inst.a.resolved) {
                op = symtab_resolve(symtab, inst.inst.a.operand.symbol);
                free(inst.inst.a.operand.symbol);
            } else {
                op = inst.inst.a.operand.address;
            }
        } else if (inst.id == INST_C) {
            INST_TO_OPCODE(inst.inst.c, op);
        }

        printf(OPCODE_STR"\n", OPCODE_TO_BINARY(op));
    }

    symtab_destroy(symtab);
    free(instructions);

    return 0;
}
