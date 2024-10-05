.z80
.code

.export _asinhf
_asinhf:
	ld hl,2
	add hl,sp
	ld bc,__floatd
	call f32asinh
	ld hl,(__floatd+2)
	ld (__hireg),hl
	ld hl,(__floatd+0)
	ret

; Adapted from z80float by Zeda Thomas (Apache license).

.export f32asinh
f32asinh:
;asinh(x) = log(x+sqrt(x^2+1))
;
  call __pushpopf
  push bc
  push hl
  ld bc,f32bgi_a
  ld d,h
  ld e,l
  call f32mul
  ld h,b
  ld l,c
  ld de,f32_const_1
  call f32add
  call f32sqrt
  pop de
  call f32add
  pop bc
  jp f32log
  