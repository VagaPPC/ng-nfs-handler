| Stub di ingresso dell'handler: DEVE essere linkato per primo, cosi' la
| prima istruzione del primo hunk e' questo salto (gli oggetti a.out di
| bebbo-gcc mettono le stringhe costanti nel .text prima del codice).

	.text
	.globl	_start
_start:
	jmp	_handler_entry
