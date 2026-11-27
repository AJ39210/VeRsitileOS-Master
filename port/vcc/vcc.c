#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TOKEN_TEXT_MAX 64
#define VARIABLE_LIMIT 256
#define ARGUMENT_LIMIT 6

typedef enum
{
    TOKEN_END,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_CHARACTER,
    TOKEN_SYMBOL
} token_kind_t;

typedef struct
{
    token_kind_t kind;
    char text[TOKEN_TEXT_MAX];
    uint64_t number;
    uint32_t line;
} token_t;

typedef struct
{
    char *data;
    size_t length;
    size_t capacity;
} buffer_t;

typedef struct
{
    char name[TOKEN_TEXT_MAX];
    unsigned slot;
} variable_t;

static const char *source;
static uint32_t source_line = 1;
static token_t current;
static token_t lookahead;
static buffer_t *output;
static variable_t variables[VARIABLE_LIMIT];
static unsigned variable_count;
static unsigned max_variable_count;
static unsigned label_count;
static unsigned return_label;

#define MACHINE_LABEL_LIMIT 4096
#define MACHINE_PATCH_LIMIT 8192
#define ELF_BASE_ADDRESS 0x400000ULL
#define VCC_EXECUTABLE_LIMIT (16ULL * 1024 * 1024)

typedef struct
{
    uint8_t *data;
    size_t length;
    size_t capacity;
} machine_code_t;

typedef struct
{
    char name[TOKEN_TEXT_MAX];
    size_t offset;
} machine_label_t;

typedef struct
{
    char name[TOKEN_TEXT_MAX];
    size_t displacement_offset;
} machine_patch_t;

static machine_label_t machine_labels[MACHINE_LABEL_LIMIT];
static machine_patch_t machine_patches[MACHINE_PATCH_LIMIT];
static size_t machine_label_count;
static size_t machine_patch_count;
static bool machine_error;

static void parser_error(const char *format, ...)
{
    va_list arguments;
    fprintf(stderr, "vcc: line %u: ", current.line);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    exit(1);
}

static void buffer_emit(buffer_t *buffer, const char *format, ...)
{
    char temporary[512];
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf(temporary, sizeof(temporary), format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= sizeof(temporary))
    {
        fputs("vcc: generated instruction is too long\n", stderr);
        exit(1);
    }

    size_t required = buffer->length + (size_t)count + 1;
    if (required > buffer->capacity)
    {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < required)
        {
            if (capacity > SIZE_MAX / 2)
            {
                fputs("vcc: generated output is too large\n", stderr);
                exit(1);
            }
            capacity *= 2;
        }
        char *data = realloc(buffer->data, capacity);
        if (!data)
        {
            fputs("vcc: out of memory\n", stderr);
            exit(1);
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }

    memcpy(buffer->data + buffer->length, temporary, (size_t)count);
    buffer->length += (size_t)count;
    buffer->data[buffer->length] = '\0';
}

static bool is_identifier_start(char character)
{
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') || character == '_';
}

static bool is_identifier_character(char character)
{
    return is_identifier_start(character) ||
        (character >= '0' && character <= '9');
}

static token_t lex_token(void)
{
    token_t token = { .kind = TOKEN_END, .line = source_line };
    for (;;)
    {
        while (*source == ' ' || *source == '\t' || *source == '\r' ||
               *source == '\n')
        {
            if (*source == '\n')
                source_line++;
            source++;
        }

        if (source[0] == '/' && source[1] == '/')
        {
            source += 2;
            while (*source && *source != '\n')
                source++;
            continue;
        }
        if (source[0] == '/' && source[1] == '*')
        {
            source += 2;
            while (*source && !(source[0] == '*' && source[1] == '/'))
            {
                if (*source == '\n')
                    source_line++;
                source++;
            }
            if (!*source)
            {
                token.line = source_line;
                current = token;
                parser_error("unterminated block comment");
            }
            source += 2;
            continue;
        }
        break;
    }

    token.line = source_line;
    if (!*source)
        return token;

    if (is_identifier_start(*source))
    {
        token.kind = TOKEN_IDENTIFIER;
        size_t length = 0;
        while (is_identifier_character(*source))
        {
            if (length + 1 >= sizeof(token.text))
            {
                current = token;
                parser_error("identifier is too long");
            }
            token.text[length++] = *source++;
        }
        token.text[length] = '\0';
        return token;
    }

    if (*source >= '0' && *source <= '9')
    {
        token.kind = TOKEN_NUMBER;
        size_t length = 0;
        while (is_identifier_character(*source))
        {
            if (length + 1 >= sizeof(token.text))
            {
                current = token;
                parser_error("integer literal is too long");
            }
            token.text[length++] = *source++;
        }
        token.text[length] = '\0';
        char *end;
        token.number = (uint64_t)strtoul(token.text, &end, 0);
        if (*end)
        {
            current = token;
            parser_error("invalid integer literal '%s'", token.text);
        }
        return token;
    }

    if (*source == '\'')
    {
        source++;
        token.kind = TOKEN_CHARACTER;
        if (!*source || *source == '\'' || *source == '\n')
        {
            current = token;
            parser_error("invalid character literal");
        }
        if (*source == '\\')
        {
            source++;
            switch (*source++)
            {
                case 'n': token.number = '\n'; break;
                case 'r': token.number = '\r'; break;
                case 't': token.number = '\t'; break;
                case '0': token.number = '\0'; break;
                case '\\': token.number = '\\'; break;
                case '\'': token.number = '\''; break;
                default:
                    current = token;
                    parser_error("unsupported escape in character literal");
            }
        }
        else
        {
            token.number = (unsigned char)*source++;
        }
        if (*source != '\'')
        {
            current = token;
            parser_error("character literal must contain one character");
        }
        source++;
        return token;
    }

    token.kind = TOKEN_SYMBOL;
    if ((source[0] == '=' && source[1] == '=') ||
        (source[0] == '!' && source[1] == '=') ||
        (source[0] == '<' && source[1] == '=') ||
        (source[0] == '>' && source[1] == '=') ||
        (source[0] == '&' && source[1] == '&') ||
        (source[0] == '|' && source[1] == '|'))
    {
        token.text[0] = source[0];
        token.text[1] = source[1];
        token.text[2] = '\0';
        source += 2;
    }
    else if (strchr("(){};,=+-*/%!~<>&|^", *source))
    {
        token.text[0] = *source++;
        token.text[1] = '\0';
    }
    else
    {
        current = token;
        parser_error("unsupported character '%c'", *source);
    }
    return token;
}

static void advance(void)
{
    current = lookahead;
    lookahead = lex_token();
}

static bool token_is(const token_t *token, const char *text)
{
    return token->kind == TOKEN_SYMBOL && strcmp(token->text, text) == 0;
}

static bool keyword_is(const token_t *token, const char *text)
{
    return token->kind == TOKEN_IDENTIFIER &&
        strcmp(token->text, text) == 0;
}

static void expect_symbol(const char *text)
{
    if (!token_is(&current, text))
        parser_error("expected '%s'", text);
    advance();
}

static void expect_identifier(char name[TOKEN_TEXT_MAX])
{
    if (current.kind != TOKEN_IDENTIFIER)
        parser_error("expected identifier");
    memcpy(name, current.text, sizeof(current.text));
    advance();
}

static int find_variable(const char *name)
{
    for (unsigned index = variable_count; index > 0; index--)
    {
        if (strcmp(variables[index - 1].name, name) == 0)
            return (int)variables[index - 1].slot;
    }
    return -1;
}

static unsigned add_variable(const char *name)
{
    if (find_variable(name) >= 0)
        parser_error("duplicate local variable '%s'", name);
    if (variable_count >= VARIABLE_LIMIT)
        parser_error("function has too many local variables");

    variable_t *variable = &variables[variable_count];
    memcpy(variable->name, name, strlen(name) + 1);
    variable->slot = variable_count;
    variable_count++;
    if (variable_count > max_variable_count)
        max_variable_count = variable_count;
    return variable->slot;
}

static void emit_load(unsigned slot)
{
    buffer_emit(output, "    mov rax, qword [rbp - %u]\n", (slot + 1) * 8);
}

static void emit_store(unsigned slot)
{
    buffer_emit(output, "    mov qword [rbp - %u], rax\n", (slot + 1) * 8);
}

static void parse_expression(void);
static void parse_assignment(void);
static void parse_statement(void);

static void parse_primary(void)
{
    if (current.kind == TOKEN_NUMBER || current.kind == TOKEN_CHARACTER)
    {
        buffer_emit(output, "    mov rax, %llu\n",
            (unsigned long long)current.number);
        advance();
        return;
    }

    if (token_is(&current, "("))
    {
        advance();
        parse_expression();
        expect_symbol(")");
        return;
    }

    if (current.kind != TOKEN_IDENTIFIER)
        parser_error("expected expression");

    char name[TOKEN_TEXT_MAX];
    memcpy(name, current.text, sizeof(name));
    advance();

    if (token_is(&current, "("))
    {
        static const char *const argument_registers[ARGUMENT_LIMIT] = {
            "rdi", "rsi", "rdx", "rcx", "r8", "r9"
        };
        advance();
        unsigned argument_count = 0;
        if (!token_is(&current, ")"))
        {
            for (;;)
            {
                if (argument_count >= ARGUMENT_LIMIT)
                    parser_error("functions may have at most six arguments");
                parse_assignment();
                buffer_emit(output, "    push rax\n");
                argument_count++;
                if (!token_is(&current, ","))
                    break;
                advance();
            }
        }
        expect_symbol(")");
        while (argument_count)
        {
            argument_count--;
            buffer_emit(output, "    pop %s\n",
                argument_registers[argument_count]);
        }
        buffer_emit(output, "    call %s\n", name);
        return;
    }

    int slot = find_variable(name);
    if (slot < 0)
        parser_error("unknown local variable '%s'", name);
    emit_load((unsigned)slot);
}

static void parse_unary(void)
{
    if (token_is(&current, "+"))
    {
        advance();
        parse_unary();
        return;
    }
    if (token_is(&current, "-"))
    {
        advance();
        parse_unary();
        buffer_emit(output, "    neg rax\n");
        return;
    }
    if (token_is(&current, "!"))
    {
        advance();
        parse_unary();
        buffer_emit(output, "    test rax, rax\n"
                            "    sete al\n"
                            "    movzx rax, al\n");
        return;
    }
    if (token_is(&current, "~"))
    {
        advance();
        parse_unary();
        buffer_emit(output, "    not rax\n");
        return;
    }
    parse_primary();
}

static void parse_binary_rhs(void (*next_level)(void), const char *const *ops,
    size_t count)
{
    next_level();
    for (;;)
    {
        size_t operation = count;
        for (size_t index = 0; index < count; index++)
        {
            if (token_is(&current, ops[index]))
            {
                operation = index;
                break;
            }
        }
        if (operation == count)
            return;

        char operator_text[3];
        memcpy(operator_text, current.text, sizeof(operator_text));
        advance();
        buffer_emit(output, "    push rax\n");
        next_level();
        buffer_emit(output, "    mov rdi, rax\n"
                            "    pop rax\n");

        if (strcmp(operator_text, "*") == 0)
            buffer_emit(output, "    imul rax, rdi\n");
        else if (strcmp(operator_text, "/") == 0)
            buffer_emit(output, "    cqo\n"
                                "    idiv rdi\n");
        else if (strcmp(operator_text, "%") == 0)
            buffer_emit(output, "    cqo\n"
                                "    idiv rdi\n"
                                "    mov rax, rdx\n");
        else if (strcmp(operator_text, "+") == 0)
            buffer_emit(output, "    add rax, rdi\n");
        else if (strcmp(operator_text, "-") == 0)
            buffer_emit(output, "    sub rax, rdi\n");
        else if (strcmp(operator_text, "<") == 0)
            buffer_emit(output, "    cmp rax, rdi\n"
                                "    setl al\n"
                                "    movzx rax, al\n");
        else if (strcmp(operator_text, ">") == 0)
            buffer_emit(output, "    cmp rax, rdi\n"
                                "    setg al\n"
                                "    movzx rax, al\n");
        else if (strcmp(operator_text, "<=") == 0)
            buffer_emit(output, "    cmp rax, rdi\n"
                                "    setle al\n"
                                "    movzx rax, al\n");
        else if (strcmp(operator_text, ">=") == 0)
            buffer_emit(output, "    cmp rax, rdi\n"
                                "    setge al\n"
                                "    movzx rax, al\n");
        else if (strcmp(operator_text, "==") == 0)
            buffer_emit(output, "    cmp rax, rdi\n"
                                "    sete al\n"
                                "    movzx rax, al\n");
        else if (strcmp(operator_text, "!=") == 0)
            buffer_emit(output, "    cmp rax, rdi\n"
                                "    setne al\n"
                                "    movzx rax, al\n");
        else if (strcmp(operator_text, "&") == 0)
            buffer_emit(output, "    and rax, rdi\n");
        else if (strcmp(operator_text, "^") == 0)
            buffer_emit(output, "    xor rax, rdi\n");
        else if (strcmp(operator_text, "|") == 0)
            buffer_emit(output, "    or rax, rdi\n");
    }
}

static void parse_multiplicative(void)
{
    static const char *const operators[] = {"*", "/", "%"};
    parse_binary_rhs(parse_unary, operators,
        sizeof(operators) / sizeof(operators[0]));
}

static void parse_additive(void)
{
    static const char *const operators[] = {"+", "-"};
    parse_binary_rhs(parse_multiplicative, operators,
        sizeof(operators) / sizeof(operators[0]));
}

static void parse_relational(void)
{
    static const char *const operators[] = {"<", ">", "<=", ">="};
    parse_binary_rhs(parse_additive, operators,
        sizeof(operators) / sizeof(operators[0]));
}

static void parse_equality(void)
{
    static const char *const operators[] = {"==", "!="};
    parse_binary_rhs(parse_relational, operators,
        sizeof(operators) / sizeof(operators[0]));
}

static void parse_bitwise_and(void)
{
    static const char *const operators[] = {"&"};
    parse_binary_rhs(parse_equality, operators,
        sizeof(operators) / sizeof(operators[0]));
}

static void parse_bitwise_xor(void)
{
    static const char *const operators[] = {"^"};
    parse_binary_rhs(parse_bitwise_and, operators,
        sizeof(operators) / sizeof(operators[0]));
}

static void parse_bitwise_or(void)
{
    static const char *const operators[] = {"|"};
    parse_binary_rhs(parse_bitwise_xor, operators,
        sizeof(operators) / sizeof(operators[0]));
}

static void parse_logical_and(void)
{
    parse_bitwise_or();
    while (token_is(&current, "&&"))
    {
        unsigned label = label_count++;
        advance();
        buffer_emit(output, "    test rax, rax\n"
                            "    jz .Llogic_false_%u\n", label);
        parse_bitwise_or();
        buffer_emit(output, "    test rax, rax\n"
                            "    setne al\n"
                            "    movzx rax, al\n"
                            "    jmp .Llogic_end_%u\n"
                            ".Llogic_false_%u:\n"
                            "    xor eax, eax\n"
                            ".Llogic_end_%u:\n",
            label, label, label);
    }
}

static void parse_logical_or(void)
{
    parse_logical_and();
    while (token_is(&current, "||"))
    {
        unsigned label = label_count++;
        advance();
        buffer_emit(output, "    test rax, rax\n"
                            "    jnz .Llogic_true_%u\n", label);
        parse_logical_and();
        buffer_emit(output, "    test rax, rax\n"
                            "    setne al\n"
                            "    movzx rax, al\n"
                            "    jmp .Llogic_end_%u\n"
                            ".Llogic_true_%u:\n"
                            "    mov eax, 1\n"
                            ".Llogic_end_%u:\n",
            label, label, label);
    }
}

static void parse_assignment(void)
{
    if (current.kind == TOKEN_IDENTIFIER &&
        token_is(&lookahead, "="))
    {
        char name[TOKEN_TEXT_MAX];
        memcpy(name, current.text, sizeof(name));
        advance();
        advance();
        int slot = find_variable(name);
        if (slot < 0)
            parser_error("unknown local variable '%s'", name);
        parse_assignment();
        emit_store((unsigned)slot);
        return;
    }
    parse_logical_or();
}

static void parse_expression(void)
{
    parse_assignment();
    while (token_is(&current, ","))
    {
        advance();
        parse_assignment();
    }
}

static bool is_type_keyword(const token_t *token)
{
    return keyword_is(token, "int") || keyword_is(token, "long") ||
        keyword_is(token, "void");
}

static void parse_block(void)
{
    expect_symbol("{");
    unsigned scope_start = variable_count;
    while (!token_is(&current, "}") && current.kind != TOKEN_END)
        parse_statement();
    expect_symbol("}");
    variable_count = scope_start;
}

static void parse_declaration(void)
{
    if (!keyword_is(&current, "int") && !keyword_is(&current, "long"))
        parser_error("only int and long local variables are supported");
    advance();
    char name[TOKEN_TEXT_MAX];
    expect_identifier(name);
    unsigned slot = add_variable(name);
    buffer_emit(output, "    mov qword [rbp - %u], 0\n", (slot + 1) * 8);
    if (token_is(&current, "="))
    {
        advance();
        parse_assignment();
        emit_store(slot);
    }
    expect_symbol(";");
}

static void parse_statement(void)
{
    if (token_is(&current, "{"))
    {
        parse_block();
        return;
    }

    if (keyword_is(&current, "return"))
    {
        advance();
        if (!token_is(&current, ";"))
            parse_expression();
        else
            buffer_emit(output, "    xor eax, eax\n");
        expect_symbol(";");
        buffer_emit(output, "    jmp .Lreturn_%u\n", return_label);
        return;
    }

    if (keyword_is(&current, "if"))
    {
        unsigned label = label_count++;
        advance();
        expect_symbol("(");
        parse_expression();
        expect_symbol(")");
        buffer_emit(output, "    test rax, rax\n"
                            "    jz .Lelse_%u\n", label);
        parse_statement();
        if (keyword_is(&current, "else"))
        {
            advance();
            buffer_emit(output, "    jmp .Lif_end_%u\n"
                                ".Lelse_%u:\n", label, label);
            parse_statement();
            buffer_emit(output, ".Lif_end_%u:\n", label);
        }
        else
        {
            buffer_emit(output, ".Lelse_%u:\n", label);
        }
        return;
    }

    if (keyword_is(&current, "while"))
    {
        unsigned label = label_count++;
        advance();
        buffer_emit(output, ".Lwhile_start_%u:\n", label);
        expect_symbol("(");
        parse_expression();
        expect_symbol(")");
        buffer_emit(output, "    test rax, rax\n"
                            "    jz .Lwhile_end_%u\n", label);
        parse_statement();
        buffer_emit(output, "    jmp .Lwhile_start_%u\n"
                            ".Lwhile_end_%u:\n", label, label);
        return;
    }

    if (is_type_keyword(&current))
    {
        parse_declaration();
        return;
    }

    if (token_is(&current, ";"))
    {
        advance();
        return;
    }

    parse_expression();
    expect_symbol(";");
}

static void emit_function(const char *name, buffer_t *body)
{
    unsigned frame_size = (max_variable_count * 8 + 15) & ~15U;
    buffer_emit(output, "\nglobal %s\n%s:\n"
                        "    push rbp\n"
                        "    mov rbp, rsp\n", name, name);
    if (frame_size)
        buffer_emit(output, "    sub rsp, %u\n", frame_size);
    size_t offset = 0;
    while (offset < body->length)
    {
        size_t count = body->length - offset;
        if (count > 400)
            count = 400;
        buffer_emit(output, "%.*s", (int)count, body->data + offset);
        offset += count;
    }
    buffer_emit(output, ".Lreturn_%u:\n"
                        "    mov rsp, rbp\n"
                        "    pop rbp\n"
                        "    ret\n", return_label);
}

static void parse_function(void)
{
    if (!is_type_keyword(&current))
        parser_error("expected a function return type");
    advance();

    char name[TOKEN_TEXT_MAX];
    expect_identifier(name);
    expect_symbol("(");

    variable_count = 0;
    max_variable_count = 0;
    buffer_t body = {0};
    buffer_t *previous_output = output;
    output = &body;

    static const char *const argument_registers[ARGUMENT_LIMIT] = {
        "rdi", "rsi", "rdx", "rcx", "r8", "r9"
    };
    unsigned argument_count = 0;
    if (keyword_is(&current, "void") && token_is(&lookahead, ")"))
    {
        advance();
    }
    else if (!token_is(&current, ")"))
    {
        for (;;)
        {
            if (!keyword_is(&current, "int") &&
                !keyword_is(&current, "long"))
                parser_error("function parameters must use int or long");
            advance();
            char parameter[TOKEN_TEXT_MAX];
            expect_identifier(parameter);
            if (argument_count >= ARGUMENT_LIMIT)
                parser_error("functions may have at most six parameters");
            unsigned slot = add_variable(parameter);
            buffer_emit(output, "    mov qword [rbp - %u], %s\n",
                (slot + 1) * 8, argument_registers[argument_count]);
            argument_count++;
            if (!token_is(&current, ","))
                break;
            advance();
        }
    }
    expect_symbol(")");
    if (!token_is(&current, "{"))
        parser_error("function prototypes are not supported");

    return_label = label_count++;
    parse_block();
    buffer_emit(output, "    xor eax, eax\n");

    output = previous_output;
    emit_function(name, &body);
    free(body.data);
}

static bool read_source(const char *path, char **contents, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "vcc: cannot open '%s'\n", path);
        return false;
    }

    size_t capacity = 4096;
    size_t used = 0;
    char *data = malloc(capacity);
    if (!data)
    {
        fclose(file);
        fputs("vcc: out of memory\n", stderr);
        return false;
    }

    for (;;)
    {
        int character = fgetc(file);
        if (character == EOF)
            break;
        if (used + 1 >= capacity)
        {
            if (capacity > SIZE_MAX / 2)
            {
                free(data);
                fclose(file);
                fputs("vcc: source file is too large\n", stderr);
                return false;
            }
            capacity *= 2;
            char *larger = realloc(data, capacity);
            if (!larger)
            {
                free(data);
                fclose(file);
                fputs("vcc: out of memory\n", stderr);
                return false;
            }
            data = larger;
        }
        data[used++] = (char)character;
    }

    bool failed = ferror(file) != 0;
    if (fclose(file) != 0)
        failed = true;
    if (failed)
    {
        free(data);
        fprintf(stderr, "vcc: error reading '%s'\n", path);
        return false;
    }
    data[used] = '\0';
    *contents = data;
    *length = used;
    return true;
}

static bool machine_reserve(machine_code_t *code, size_t extra)
{
    if (extra > SIZE_MAX - code->length)
        return false;
    size_t required = code->length + extra;
    if (required <= code->capacity)
        return true;

    size_t capacity = code->capacity ? code->capacity : 256;
    while (capacity < required)
    {
        if (capacity > SIZE_MAX / 2)
        {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    uint8_t *data = realloc(code->data, capacity);
    if (!data)
        return false;
    code->data = data;
    code->capacity = capacity;
    return true;
}

static void machine_emit8(machine_code_t *code, uint8_t value)
{
    if (!machine_reserve(code, 1))
    {
        machine_error = true;
        return;
    }
    code->data[code->length++] = value;
}

static void machine_emit32(machine_code_t *code, uint32_t value)
{
    for (unsigned index = 0; index < 4; index++)
        machine_emit8(code, (uint8_t)(value >> (index * 8)));
}

static void machine_emit64(machine_code_t *code, uint64_t value)
{
    for (unsigned index = 0; index < 8; index++)
        machine_emit8(code, (uint8_t)(value >> (index * 8)));
}

static bool machine_add_label(const char *name, size_t offset)
{
    if (machine_label_count >= MACHINE_LABEL_LIMIT ||
        strlen(name) >= sizeof(machine_labels[0].name))
        return false;
    for (size_t index = 0; index < machine_label_count; index++)
    {
        if (strcmp(machine_labels[index].name, name) == 0)
            return false;
    }
    machine_label_t *label = &machine_labels[machine_label_count++];
    memcpy(label->name, name, strlen(name) + 1);
    label->offset = offset;
    return true;
}

static bool machine_add_patch(const char *name, size_t offset)
{
    if (machine_patch_count >= MACHINE_PATCH_LIMIT ||
        strlen(name) >= sizeof(machine_patches[0].name))
        return false;
    machine_patch_t *patch = &machine_patches[machine_patch_count++];
    memcpy(patch->name, name, strlen(name) + 1);
    patch->displacement_offset = offset;
    return true;
}

static void machine_emit_relative(machine_code_t *code, const char *target,
    uint8_t opcode, bool conditional)
{
    if (conditional)
        machine_emit8(code, 0x0F);
    machine_emit8(code, opcode);
    size_t displacement_offset = code->length;
    machine_emit32(code, 0);
    if (!machine_add_patch(target, displacement_offset))
        machine_error = true;
}

static int machine_register(const char *name)
{
    static const char *const names[] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9"
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++)
    {
        if (strcmp(name, names[index]) == 0)
            return (int)index;
    }
    return -1;
}

static bool parse_stack_offset(const char *operand, uint32_t *offset)
{
    static const char prefix[] = "qword[rbp-";
    if (strncmp(operand, prefix, sizeof(prefix) - 1) != 0)
        return false;
    char *end;
    unsigned long value = strtoul(operand + sizeof(prefix) - 1, &end, 10);
    if (*end != ']' || value == 0 || value > INT32_MAX)
        return false;
    *offset = (uint32_t)value;
    return true;
}

static void encode_stack_memory(machine_code_t *code, bool load,
    uint32_t offset, int register_code)
{
    uint32_t displacement = (uint32_t)(-(int32_t)offset);
    bool short_displacement = offset <= 128;
    if (register_code >= 8)
        machine_emit8(code, 0x4C);
    else
        machine_emit8(code, 0x48);
    machine_emit8(code, load ? 0x8B : 0x89);
    uint8_t mod = short_displacement ? 0x40 : 0x80;
    machine_emit8(code,
        (uint8_t)(mod | ((register_code & 7) << 3) | 5));
    if (short_displacement)
        machine_emit8(code, (uint8_t)displacement);
    else
        machine_emit32(code, displacement);
}

static void encode_stack_immediate(machine_code_t *code, uint32_t offset,
    uint32_t immediate)
{
    uint32_t displacement = (uint32_t)(-(int32_t)offset);
    bool short_displacement = offset <= 128;
    machine_emit8(code, 0x48);
    machine_emit8(code, 0xC7);
    machine_emit8(code, short_displacement ? 0x45 : 0x85);
    if (short_displacement)
        machine_emit8(code, (uint8_t)displacement);
    else
        machine_emit32(code, displacement);
    machine_emit32(code, immediate);
}

static void machine_error_line(const char *line)
{
    fprintf(stderr, "vcc: cannot assemble generated instruction: %s\n", line);
    machine_error = true;
}

static bool encode_instruction(machine_code_t *code, char *line)
{
    char normalized[256];
    size_t output_length = 0;
    for (size_t index = 0; line[index]; index++)
    {
        if (line[index] != ' ' && line[index] != '\t' &&
            line[index] != '\r')
        {
            if (output_length + 1 >= sizeof(normalized))
                return false;
            normalized[output_length++] = line[index];
        }
    }
    normalized[output_length] = '\0';
    if (!output_length || strncmp(normalized, "BITS", 4) == 0 ||
        strncmp(normalized, "DEFAULT", 7) == 0 ||
        strncmp(normalized, "SECTION", 7) == 0 ||
        strncmp(normalized, "global", 6) == 0)
        return true;

    if (normalized[output_length - 1] == ':')
    {
        normalized[output_length - 1] = '\0';
        return machine_add_label(normalized, code->length);
    }

    if (strncmp(normalized, "movrax,", 7) == 0)
    {
        const char *operand = normalized + 7;
        uint32_t offset;
        if (parse_stack_offset(operand, &offset))
        {
            encode_stack_memory(code, true, offset, 0);
            return true;
        }
        int source_register = machine_register(operand);
        if (source_register >= 0)
        {
            machine_emit8(code, source_register >= 8 ? 0x49 : 0x48);
            machine_emit8(code, 0x89);
            machine_emit8(code,
                (uint8_t)(0xC0 | ((source_register & 7) << 3)));
            return true;
        }
        char *end;
        uint64_t immediate = strtoul(operand, &end, 0);
        if (*end)
            return false;
        machine_emit8(code, 0x48);
        machine_emit8(code, 0xB8);
        machine_emit64(code, immediate);
        return true;
    }

    if (strncmp(normalized, "movqword[rbp-", 13) == 0)
    {
        char *close = strchr(normalized, ']');
        if (!close || close[1] != ',')
            return false;
        const char *source_register = close + 2;
        char comma = close[1];
        close[1] = '\0';
        uint32_t offset;
        bool valid_offset = parse_stack_offset(normalized + 3, &offset);
        close[1] = comma;
        if (!valid_offset)
            return false;
        if (strcmp(source_register, "0") == 0)
        {
            encode_stack_immediate(code, offset, 0);
            return true;
        }
        int register_code = machine_register(source_register);
        if (register_code < 0)
            return false;
        encode_stack_memory(code, false, offset, register_code);
        return true;
    }

    if (strncmp(normalized, "mov", 3) == 0 &&
        strncmp(normalized, "movzx", 5) != 0)
    {
        char *comma = strchr(normalized + 3, ',');
        if (!comma)
            return false;
        *comma++ = '\0';
        const char *destination = normalized + 3;
        if (strcmp(destination, "eax") == 0)
        {
            char *end;
            unsigned long immediate = strtoul(comma, &end, 0);
            if (*end || immediate > UINT32_MAX)
                return false;
            machine_emit8(code, 0xB8);
            machine_emit32(code, (uint32_t)immediate);
            return true;
        }
        int destination_code = machine_register(destination);
        int source_code = machine_register(comma);
        if (destination_code < 0 || source_code < 0)
            return false;
        if (destination_code == 0 && source_code == 2)
        {
            machine_emit8(code, 0x48);
            machine_emit8(code, 0x89);
            machine_emit8(code, 0xD0);
            return true;
        }
        machine_emit8(code, (destination_code >= 8 || source_code >= 8)
            ? 0x49 : 0x48);
        machine_emit8(code, 0x89);
        machine_emit8(code,
            (uint8_t)(0xC0 | ((source_code & 7) << 3) |
                (destination_code & 7)));
        return true;
    }

    if (strncmp(normalized, "subrsp,", 7) == 0)
    {
        char *end;
        unsigned long immediate = strtoul(normalized + 7, &end, 0);
        if (*end || immediate > INT32_MAX)
            return false;
        machine_emit8(code, 0x48);
        if (immediate <= 127)
        {
            machine_emit8(code, 0x83);
            machine_emit8(code, 0xEC);
            machine_emit8(code, (uint8_t)immediate);
        }
        else
        {
            machine_emit8(code, 0x81);
            machine_emit8(code, 0xEC);
            machine_emit32(code, (uint32_t)immediate);
        }
        return true;
    }

    if (strcmp(normalized, "pushrbp") == 0)
        machine_emit8(code, 0x55);
    else if (strcmp(normalized, "poprbp") == 0)
        machine_emit8(code, 0x5D);
    else if (strcmp(normalized, "pushrax") == 0)
        machine_emit8(code, 0x50);
    else if (strcmp(normalized, "poprax") == 0)
        machine_emit8(code, 0x58);
    else if (strcmp(normalized, "poprdi") == 0)
        machine_emit8(code, 0x5F);
    else if (strcmp(normalized, "poprsi") == 0)
        machine_emit8(code, 0x5E);
    else if (strcmp(normalized, "poprdx") == 0)
        machine_emit8(code, 0x5A);
    else if (strcmp(normalized, "poprcx") == 0)
        machine_emit8(code, 0x59);
    else if (strcmp(normalized, "popr8") == 0)
    {
        machine_emit8(code, 0x41);
        machine_emit8(code, 0x58);
    }
    else if (strcmp(normalized, "popr9") == 0)
    {
        machine_emit8(code, 0x41);
        machine_emit8(code, 0x59);
    }
    else if (strcmp(normalized, "movrbp,rsp") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0x89);
        machine_emit8(code, 0xE5);
    }
    else if (strcmp(normalized, "movrsp,rbp") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0x89);
        machine_emit8(code, 0xEC);
    }
    else if (strcmp(normalized, "testrax,rax") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0x85);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "seteal") == 0)
    {
        machine_emit8(code, 0x0F);
        machine_emit8(code, 0x94);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "setneal") == 0)
    {
        machine_emit8(code, 0x0F);
        machine_emit8(code, 0x95);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "setlal") == 0)
    {
        machine_emit8(code, 0x0F);
        machine_emit8(code, 0x9C);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "setgal") == 0)
    {
        machine_emit8(code, 0x0F);
        machine_emit8(code, 0x9F);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "setleal") == 0)
    {
        machine_emit8(code, 0x0F);
        machine_emit8(code, 0x9E);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "setgeal") == 0)
    {
        machine_emit8(code, 0x0F);
        machine_emit8(code, 0x9D);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "movzxrax,al") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0x0F);
        machine_emit8(code, 0xB6);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "negrax") == 0 ||
             strcmp(normalized, "notrax") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0xF7);
        machine_emit8(code,
            strcmp(normalized, "negrax") == 0 ? 0xD8 : 0xD0);
    }
    else if (strcmp(normalized, "imulrax,rdi") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0x0F);
        machine_emit8(code, 0xAF);
        machine_emit8(code, 0xC7);
    }
    else if (strcmp(normalized, "cqo") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0x99);
    }
    else if (strcmp(normalized, "idivrdi") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0xF7);
        machine_emit8(code, 0xFF);
    }
    else if (strcmp(normalized, "addrax,rdi") == 0 ||
             strcmp(normalized, "subrax,rdi") == 0 ||
             strcmp(normalized, "cmprax,rdi") == 0 ||
             strcmp(normalized, "andrax,rdi") == 0 ||
             strcmp(normalized, "xorrax,rdi") == 0 ||
             strcmp(normalized, "orrax,rdi") == 0)
    {
        machine_emit8(code, 0x48);
        if (strcmp(normalized, "addrax,rdi") == 0)
            machine_emit8(code, 0x01);
        else if (strcmp(normalized, "subrax,rdi") == 0)
            machine_emit8(code, 0x29);
        else if (strcmp(normalized, "cmprax,rdi") == 0)
            machine_emit8(code, 0x39);
        else if (strcmp(normalized, "andrax,rdi") == 0)
            machine_emit8(code, 0x21);
        else if (strcmp(normalized, "xorrax,rdi") == 0)
            machine_emit8(code, 0x31);
        else
            machine_emit8(code, 0x09);
        machine_emit8(code, 0xF8);
    }
    else if (strcmp(normalized, "movrax,rdx") == 0)
    {
        machine_emit8(code, 0x48);
        machine_emit8(code, 0x89);
        machine_emit8(code, 0xD0);
    }
    else if (strcmp(normalized, "xoreax,eax") == 0)
    {
        machine_emit8(code, 0x31);
        machine_emit8(code, 0xC0);
    }
    else if (strcmp(normalized, "ret") == 0)
        machine_emit8(code, 0xC3);
    else if (strcmp(normalized, "int0x80") == 0)
    {
        machine_emit8(code, 0xCD);
        machine_emit8(code, 0x80);
    }
    else if (strncmp(normalized, "call", 4) == 0)
    {
        machine_emit_relative(code, normalized + 4, 0xE8, false);
    }
    else if (strncmp(normalized, "jmp", 3) == 0)
    {
        machine_emit_relative(code, normalized + 3, 0xE9, false);
    }
    else if (strncmp(normalized, "jz", 2) == 0)
    {
        machine_emit_relative(code, normalized + 2, 0x84, true);
    }
    else if (strncmp(normalized, "jnz", 3) == 0)
    {
        machine_emit_relative(code, normalized + 3, 0x85, true);
    }
    else
        return false;
    return !machine_error;
}

static bool assemble_generated(const buffer_t *assembly, machine_code_t *code)
{
    machine_label_count = 0;
    machine_patch_count = 0;
    machine_error = false;
    size_t start = 0;
    while (start < assembly->length)
    {
        size_t end = start;
        while (end < assembly->length && assembly->data[end] != '\n')
            end++;
        size_t length = end - start;
        if (length >= 256)
            return false;
        char line[256];
        memcpy(line, assembly->data + start, length);
        line[length] = '\0';
        if (!encode_instruction(code, line))
        {
            machine_error_line(line);
            return false;
        }
        start = end + 1;
    }

    for (size_t index = 0; index < machine_patch_count; index++)
    {
        machine_patch_t *patch = &machine_patches[index];
        size_t target = SIZE_MAX;
        for (size_t label_index = 0; label_index < machine_label_count;
             label_index++)
        {
            if (strcmp(machine_labels[label_index].name, patch->name) == 0)
            {
                target = machine_labels[label_index].offset;
                break;
            }
        }
        if (target == SIZE_MAX)
        {
            fprintf(stderr, "vcc: undefined function or label '%s'\n",
                patch->name);
            return false;
        }
        int64_t displacement = (int64_t)target -
            (int64_t)(patch->displacement_offset + sizeof(uint32_t));
        if (displacement < INT32_MIN || displacement > INT32_MAX)
            return false;
        uint32_t encoded = (uint32_t)(int32_t)displacement;
        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++)
            code->data[patch->displacement_offset + byte] =
                (uint8_t)(encoded >> (byte * 8));
    }
    return !machine_error;
}

typedef struct __attribute__((packed))
{
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
} elf_header_t;

typedef struct __attribute__((packed))
{
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} elf_program_header_t;

static bool create_executable(const buffer_t *assembly, uint8_t **image,
    size_t *image_size)
{
    machine_code_t code = {0};
    machine_emit8(&code, 0xE8);
    size_t call_displacement = code.length;
    machine_emit32(&code, 0);
    machine_emit8(&code, 0x89);
    machine_emit8(&code, 0xC7);
    machine_emit8(&code, 0xB8);
    machine_emit32(&code, 9);
    machine_emit8(&code, 0xCD);
    machine_emit8(&code, 0x80);
    machine_emit8(&code, 0xF3);
    machine_emit8(&code, 0x90);
    machine_emit8(&code, 0xEB);
    machine_emit8(&code, 0xFC);
    if (!assemble_generated(assembly, &code))
    {
        free(code.data);
        return false;
    }

    size_t main_label = SIZE_MAX;
    for (size_t index = 0; index < machine_label_count; index++)
    {
        if (strcmp(machine_labels[index].name, "main") == 0)
        {
            main_label = machine_labels[index].offset;
            break;
        }
    }
    if (main_label == SIZE_MAX)
    {
        fputs("vcc: source must define main()\n", stderr);
        free(code.data);
        return false;
    }
    int64_t main_displacement = (int64_t)main_label -
        (int64_t)(call_displacement + sizeof(uint32_t));
    uint32_t encoded_displacement = (uint32_t)(int32_t)main_displacement;
    for (unsigned byte = 0; byte < sizeof(uint32_t); byte++)
        code.data[call_displacement + byte] =
            (uint8_t)(encoded_displacement >> (byte * 8));

    if (code.length > SIZE_MAX - sizeof(elf_header_t) -
        sizeof(elf_program_header_t))
    {
        free(code.data);
        return false;
    }
    size_t total_size = sizeof(elf_header_t) +
        sizeof(elf_program_header_t) + code.length;
    if (total_size > VCC_EXECUTABLE_LIMIT)
    {
        fputs("vcc: generated executable exceeds the OS ELF size limit\n",
            stderr);
        free(code.data);
        return false;
    }
    uint8_t *result = calloc(1, total_size);
    if (!result)
    {
        free(code.data);
        return false;
    }

    elf_header_t header = {
        .ident = {
            0x7F, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
        },
        .type = 2,
        .machine = 62,
        .version = 1,
        .entry = ELF_BASE_ADDRESS + sizeof(header) +
            sizeof(elf_program_header_t),
        .program_header_offset = sizeof(header),
        .header_size = sizeof(header),
        .program_header_size = sizeof(elf_program_header_t),
        .program_header_count = 1
    };
    elf_program_header_t program = {
        .type = 1,
        .flags = 5,
        .offset = 0,
        .virtual_address = ELF_BASE_ADDRESS,
        .physical_address = ELF_BASE_ADDRESS,
        .file_size = total_size,
        .memory_size = total_size,
        .alignment = 4096
    };
    memcpy(result, &header, sizeof(header));
    memcpy(result + sizeof(header), &program, sizeof(program));
    memcpy(result + sizeof(header) + sizeof(program), code.data, code.length);
    free(code.data);
    *image = result;
    *image_size = total_size;
    return true;
}

static void print_help(void)
{
    puts("vcc - VeRsitileOS C compiler");
    puts("Usage: vcc source.c [-o program] | vcc -S source.c");
    puts("Creates a runnable x86-64 ELF in the RAM filesystem.");
    puts("Without -o, output is /tmp/a.out; run it with '/tmp/a.out'.");
    puts("Supported: int/long functions, local integers, calls,");
    puts("arithmetic, comparisons, bitwise/logical ops, if, while, return.");
    puts("Preprocessor, pointers, arrays, strings and output files are not");
    puts("supported yet.");
}

static size_t split_os_arguments(int argc, char **argv, char *arguments[],
    size_t capacity)
{
    if (argc != 2 || !argv[1])
    {
        size_t count = 0;
        for (int index = 1; index < argc && count < capacity; index++)
            arguments[count++] = argv[index];
        return count;
    }

    size_t count = 0;
    char *cursor = argv[1];
    while (*cursor && count < capacity)
    {
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (!*cursor)
            break;
        arguments[count++] = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t')
            cursor++;
        if (*cursor)
            *cursor++ = '\0';
    }
    return count;
}

static bool write_output(const char *path, const uint8_t *data, size_t size)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (descriptor < 0)
    {
        fprintf(stderr, "vcc: cannot create '%s'\n", path);
        return false;
    }

    size_t written = 0;
    while (written < size)
    {
        ssize_t result = write(descriptor, data + written, size - written);
        if (result <= 0)
        {
            fprintf(stderr, "vcc: failed writing '%s'\n", path);
            close(descriptor);
            return false;
        }
        written += (size_t)result;
    }

    if (close(descriptor) != 0)
    {
        fprintf(stderr, "vcc: failed closing '%s'\n", path);
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    char *arguments[8];
    size_t argument_count = split_os_arguments(argc, argv, arguments,
        sizeof(arguments) / sizeof(arguments[0]));
    const char *input_path = NULL;
    const char *output_path = "/tmp/a.out";
    bool assembly_only = false;
    for (size_t index = 0; index < argument_count; index++)
    {
        if (strcmp(arguments[index], "--help") == 0 ||
            strcmp(arguments[index], "-h") == 0)
        {
            print_help();
            return 0;
        }
        if (strcmp(arguments[index], "-S") == 0)
        {
            assembly_only = true;
            continue;
        }
        if (strcmp(arguments[index], "-o") == 0)
        {
            if (++index >= argument_count || !arguments[index][0])
            {
                fputs("vcc: -o requires an output path\n", stderr);
                return 1;
            }
            output_path = arguments[index];
            continue;
        }
        if (arguments[index][0] == '-')
        {
            fprintf(stderr, "vcc: unsupported option '%s'\n",
                arguments[index]);
            return 1;
        }
        if (input_path)
        {
            fputs("vcc: provide one source file at a time\n", stderr);
            return 1;
        }
        input_path = arguments[index];
    }

    if (!input_path)
    {
        fputs("vcc: missing source file (try 'vcc --help')\n", stderr);
        return 1;
    }

    char *contents;
    size_t length;
    if (!read_source(input_path, &contents, &length))
        return 1;
    (void)length;

    buffer_t generated = {0};
    output = &generated;
    buffer_emit(output, "BITS 64\n"
                        "DEFAULT REL\n"
                        "SECTION .text\n");

    source = contents;
    source_line = 1;
    current = lex_token();
    lookahead = lex_token();
    while (current.kind != TOKEN_END)
        parse_function();

    bool failed;
    if (assembly_only)
    {
        failed = fwrite(generated.data, 1, generated.length, stdout) !=
            generated.length;
    }
    else
    {
        uint8_t *image;
        size_t image_size;
        if (!create_executable(&generated, &image, &image_size))
        {
            free(contents);
            free(generated.data);
            return 1;
        }
        failed = !write_output(output_path, image, image_size);
        free(image);
        if (!failed)
            printf("vcc: created %s (%llu bytes)\n", output_path,
                (unsigned long long)image_size);
    }
    free(contents);
    free(generated.data);
    if (failed)
    {
        fputs("vcc: compilation output failed\n", stderr);
        return 1;
    }
    return 0;
}
