add_subdirectory(parse)

target_sources(compiler PRIVATE
	codegen.c
	emit.c
	ir.c
	lex.c
	parse.c
	sema.c
)
