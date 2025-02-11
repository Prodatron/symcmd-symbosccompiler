/*
 * Output FUZIX object files for 8/16bit machines.
 *
 * Currently we understand little endian 8 and 16bit formats along with
 * PC relative.
 *
 * FIXME: We need to manage dot[segment] properly for word addressed machines
 * so that we track and write in words not bytes. Right now word addressing
 * is broken
 */

static addr_t segsize[OSEG];
static uint32_t truesize[OSEG];
static off_t segbase[OSEG];
static uint16_t segpad[OSEG];
static uint8_t full[OSEG];	/* So we can tell a full wrap from a 0 start */

static addr_t mask = 0xFFFF;
static int_fast8_t shift = 0;
static unsigned rel_check = 0;

static struct objhdr obh;

static void dumpseginfo(void)
{
#if 0
	int i;
	printf("Pass %d:\n", pass);
	for (i = 0; i < 8; i++) {
		printf("\t%d: %04X %d@%ld (pad %d)\n", i, truesize[i],
				segsize[i], segbase[i], segpad[i]);
	}
#endif
}

static void numbersymbols(void);

int outpass(void)
{
	off_t base = sizeof(obh);
	int i;

	/* Skip any passes we don't want to do for this platform */
	if (!passbegin(pass))
		return 0;

	if (pass)
		dumpseginfo();
	/* Pass 2 locks everything down so we can then set up truesize. For
	   a 2 pass (0/3 CPU) we are fine as pass 0 will lock down and truesize
	   is already clear */

	if (pass < 3) {
		for (i = 0; i < OSEG; i++) {
			truesize[i] = 0;
			segsize[i] = 0;
			segpad[i] = 0;
		}
	}

	if (pass == 3) {
		/* Lay the file out */
		for (i = 0; i < OSEG; i++) {
			segbase[i] = base;
			if (i != BSS) {
				obh.o_segbase[i] = base;
				base += segsize[i] + segpad[i] + 2; /* 2 for the EOF mark */
			}
			obh.o_size[i] = truesize[i];
			/* This will then count up again so we don't get
			   a bogus segment limit error */
			truesize[i] = 0;
			segsize[i] = 0;
		}
		obh.o_magic = 0;
		obh.o_symbase = base;
		obh.o_dbgbase = 0;	/* for now */
		/* Number the symbols for output */
		numbersymbols();
		dumpseginfo();
		outsegment(segment);
	}
	return 1;
}

/*
 * Absolute address change
 */

void outabsolute(addr_t addr)
{
	if (segment != ABSOLUTE)
		qerr(MUST_BE_ABSOLUTE);
	else {
		outbyte(REL_ESC);
		outbyte(REL_ORG);
		outbyte(addr);
		outbyte(addr >> 8);
#ifdef ARCH32
		outbyte(addr >> 16);
		outbyte(addr >> 24);
#endif
	}
}

/*
 * Segment change
 */

void outsegment(int seg)
{
	/* Seek to the current writing address for this segment */
	if (pass == 3) {
//		fprintf(stderr, "Writing segment %d(%x) at %ld\n",
//			seg, segsize[seg],(long)segbase[seg]);
		fseek(ofp, segbase[seg], SEEK_SET);
	}
}

/*
 * Output a word. Use the target word order with padding as needed
 */
static void check_store_allowed(uint8_t segment, uint16_t value)
{
	if (value == 0)
		return;
	if (segment == BSS)
		err('b', DATA_IN_BSS);
}

/* Write an address out according to size */
static void outaddr(addr_t a, unsigned s)
{
#ifdef TARGET_BIGENDIAN
	unsigned shift = 8 * (s - 1);
	while(s--) {
		outab(a >> shift);
		a <<= 8;
	}
#else
	while(s--) {
		outab(a);
		a >>= 8;
	}
#endif
}

/* Ditto allowing for quoting changes */
static void outqaddr(unsigned short a, unsigned char s)
{
    if (s == 1) {
        outabyte(a);
    } else {
        outabyte(a);
        outabyte(a >> 8);
    }
}

static void outqcaddr(unsigned short a, unsigned char s)
{
    if (s == 1) {
        outab2(a);
    } else {
        outab2(a);
        outab2(a >> 8);
    }
}

/*
 *	Symbol numbers and relocations are always written little endian
 *	for simplicity.
 *
 *	A_LOW and A_HIGH indicate 8bit partial relocations. We handle these
 *	internally.
 */
static void outreloc(ADDR *a, int bytes)
{
	int s = (bytes - 1) << 4;
	unsigned short n = a->a_value;
	unsigned short f = a->a_flags;

	/* We must insert a relocation record for anything relocatable,
	   but also for anything which is a symbol, as the linker may
	   need to do absolute resolution between modules */
	if (a->a_segment != ABSOLUTE || a->a_sym) {
		outbyte(REL_ESC);
		check_store_allowed(segment, 1);
#ifdef TARGET_RELOC_OVERFLOW_OK
		outbyte(REL_OVERFLOW);
#endif
		/* TODO: Better error ? */
		if (bytes != 1 && (f & (A_LOW|A_HIGH)))
			qerr(CONSTANT_RANGE);
		/* low bits of 16 bit is an 8bit relocation with
		   overflow suppressed */
		if (f & A_LOW) {
#ifndef TARGET_RELOC_OVERFLOW_OK
			outbyte(REL_OVERFLOW);
#endif
			s = 0 << 4;
			n &= 0xFF;
		}
		if (f & A_HIGH) {
			outbyte(REL_HIGH);
			/* High relocations are two byte to allow the linker
			   to calculate carry between the low/high byte */
			s = 1 << 4;
		}
		if (a->a_sym == NULL) {
			/* low bits of 16 bit is an 8bit relocation with
			   overflow suppressed */
			/* Trap bogus relocation records */
			if (a->a_segment == 15)
				aerr(PHASE_ERROR);
			outbyte(s | REL_SIMPLE | a->a_segment);
		} else {
			outbyte(s | REL_SYMBOL);
			outbyte(a->a_sym->s_number & 0xFF);
			outbyte(a->a_sym->s_number >> 8);
		}
		/* Relocatable constant, store unquoted as know the size */
		if (f & A_LOW)
			outabyte(n);
		else if (f & A_HIGH) {
			/* FIXME: > 2 bytes */
			outbyte(n);
			outabyte(n >> 8);
		} else
			outqaddr(n, bytes);
	} else {
		/* Relocatable constant. This may change and thus need
		   to be padded */
		if (shift) {
			if (shift < 0)
				n <<= shift;
			else
				n >>= shift;
		}
		if (rel_check && (n & ~mask))
			err('o', CONSTANT_RANGE);
		n &= mask;

		/* FIXME: need to deal with 24/32bit sizes wit HIGH */
		if (f & A_HIGH) {
			outab2(n >> 8);
		} else if (f & A_LOW) {
			if (bytes == 1)
				outab2(n & 0xFF);
			else
				outab2(n & 0xFF);
		} else {
			if (bytes == 1)
				outab(n);
			else
				outqcaddr(n, bytes);
		}
	}
}

void outraw(ADDR *a)
{
	outreloc(a, 2);
}

void outrab(ADDR *a)
{
	outreloc(a, 1);
}

/*
 * Check the bytes being added actually fit.
 */

void sizecheck(int segment)
{
	if (full[segment])
		err('o', SEGMENT_OVERFLOW);
	if (pass == 3 && (truesize[segment] == SEGMENT_LIMIT || dot[segment] == SEGMENT_LIMIT))
		full[segment] = 1;
}

/*
 * Output an absolute
 * byte to the code and listing
 * streams.
 */
void outab(uint8_t b)
{
	/* Not allowed to put data in the BSS except zero */
	check_store_allowed(segment, b);
	sizecheck(segment);
	outbyte(b);
	if (b == REL_ESC)	/* Quote relocation markers */
		outbyte(REL_REL);
	++dot[segment];
	++truesize[segment];
	list_addbyte(b);
}

/*
 * Output an absolute unquoted program byte to the code and listing
 * streams.
 */
void outabyte(uint8_t b)
{
	/* Not allowed to put data in the BSS except zero */
	check_store_allowed(segment, b);
	sizecheck(segment);
	outbyte(b);
	++dot[segment];
	++truesize[segment];
	list_addbyte(b);
}

/*
 * Output an absolute byte to the code and listing
 * streams. This version is used when the caller does not know the final
 * value until the last pass. In that case we allocate an extra byte for
 * unquoted values just in case. As the stream has an EOF marker a little bit
 * of padding is fine and does no harm.
 */
void outab2(uint8_t b)
{
	/* Not allowed to put data in the BSS except zero */
	check_store_allowed(segment, b);
	sizecheck(segment);
	outbyte(b);
	if (b == REL_ESC)	/* Quote relocation markers */
		outbyte(REL_REL);
	else
		reservebyte();
	++dot[segment];
	++truesize[segment];
	list_addbyte(b);
}

void outabchk2(ADDR *a)
{
	uint8_t b;
	unsigned short v = a->a_value;
	if (a->a_flags & A_LOW)
		b = v;
	else if (a->a_flags & A_HIGH)
		b = v >> 8;
	else  {
		if (v > 255)
			err('o', CONSTANT_RANGE);
		b = v;
	}
	outab(b);
}

void outrabrel(ADDR *a)
{
	int16_t v = (int16_t)a->a_value;
	check_store_allowed(segment, 1);
	if (a->a_sym) {
		outbyte(REL_ESC);
		outbyte((0 << 4 ) | REL_PCREL);
		outbyte(a->a_sym->s_number & 0xFF);
		outbyte(a->a_sym->s_number >> 8);
		/* TODO: look into ordering here */
		outabyte(a->a_value);
		outbyte(a->a_value >> 8);
		return;
	}
	if (shift) {
		if (shift < 0)
			v <<= shift;
		else
			v >>= shift;
	}
	if (rel_check && (v & ~mask))
		err('o', CONSTANT_RANGE);
	v &= mask;
	if (v < -128 || v > 127)
		err('o', CONSTANT_RANGE);
	/* relatives without a symbol don't need relocation but they
	   still may need a pad byte */
	outab2(v);
}

/* Write a segment relative */
void outrel(ADDR *a, unsigned len)
{
	addr_t av = (addr_t)a->a_value;
	unsigned sc = (len - 1) << 4;
	check_store_allowed(segment, 1);
	if (a->a_sym) {
		outbyte(REL_ESC);
		outbyte(sc | REL_PCREL);
		outbyte(a->a_sym->s_number & 0xFF);
		outbyte(a->a_sym->s_number >> 8);
		outaddr(av, len);
		return;
	} else if (segment == a->a_segment) {
		/* We don't need to issue a relocation if it's within
		   segment */
		av -= dot[segment];
		if (shift) {
			if (shift < 0)
				av <<= shift;
			else
				av >>= shift;
		}
		if (rel_check && (av & ~mask))
			err('o', CONSTANT_RANGE);
		av &= mask;
		outaddr(av, len);
	} else {
		outbyte(REL_ESC);
		outbyte(REL_PCR);
		outbyte(sc | REL_SIMPLE | a->a_segment);
		outaddr(av, len);
	}
}

void outrawrel(ADDR *a)
{
	outrel(a, 2);
}

static void putsymbol(SYM *s, FILE *ofp)
{
	uint8_t flag = 0;
	if (s->s_type == TNEW)
		flag |= S_UNKNOWN;
	else {
		if (s->s_type & TPUBLIC)
			flag |= S_PUBLIC;
	}
	/* 0 absolute, 1-n segments, 15 don't care */
	flag |= (s->s_segment & S_SEGMENT);
	putc(flag, ofp);
	fwrite(s->s_id, NAMELEN, 1, ofp);
	putc(s->s_value, ofp);
	putc(s->s_value >> 8, ofp);
#ifdef ARCH32
	putc(s->s_value >> 16, ofp);
	putc(s->s_value >> 24, ofp);
#endif
}

static void enumerate(SYM *s, FILE *dummy)
{
	static int sym = 0;
	s->s_number = sym++;
}

static void dosymbols(SYM *hash[], FILE *ofp, int flag, void (*op)(SYM *, FILE *f))
{
	int i;
	for (i = 0; i < NHASH; i++) {
		SYM *s;
		for (s = hash[i]; s != NULL; s = s->s_fp) {
			int t = s->s_type & TMMODE;
			int n;
			if (t != TUSER && t != TNEW)
				continue;
			n =  (t == TNEW) || (t == TUSER && (s->s_type & TPUBLIC));
			if (n == flag)
				op(s, ofp);
		}
	}
}

static void writesymbols(SYM *hash[], FILE *ofp)
{
	fseek(ofp, obh.o_symbase, SEEK_SET);
	dosymbols(hash, ofp, 1, putsymbol);
	obh.o_dbgbase = ftell(ofp);
	if (debug_write) {
		dosymbols(uhash, ofp, 0, putsymbol);
	}
}

static void numbersymbols(void)
{
	dosymbols(uhash, NULL, 1, enumerate);
}

/*
 * Put out the end of file
 * hex item at the very end of
 * the object file.
 */
void outeof(void)
{
	int i;

	/* We don't do the final write out if there was an error. That
	   leaves the magic wrong on the object file so it can't be used */
	if (noobj || pass < 3)
		return;

	/* Target specific flush of anything pending */
	doflush();

	dumpseginfo();
	for (i = 0; i < OSEG; i++) {
		/* The BSS is not written out */
		if (i == BSS)
			continue;
		segment = i;
		outsegment(i);
		outbyte(REL_ESC);
		outbyte(REL_EOF);
	}
	writesymbols(uhash, ofp);
	rewind(ofp);
	obh.o_magic = MAGIC_OBJ;
	fwrite(&obh, sizeof(obh), 1, ofp);
//	printf("Abs %d bytes: Code %d bytes: Data %d bytes: BSS %d bytes\n",
//		truesize[ABSOLUTE], truesize[CODE], truesize[DATA], truesize[BSS]);
}

/*
 * Output a byte and track our position. For BSS we care about sizes
 * only.
 */
void outbyte(uint8_t b)
{
	if (pass == 3 && segment != BSS)
		putc(b, ofp);
	segbase[segment]++;
	segsize[segment]++;
}

/* Reserve space for a possible size change on the final pass */
void reservebyte(void)
{
	segpad[segment]++;
}

void outscale(unsigned s, unsigned m)
{
	outbyte(REL_ESC);
	outbyte(REL_MOD);
	outbyte(m);
	outbyte(s);
	mask =  (1UL << (m & 0x1F)) - 1;
	if (m & 0x80)
		mask = ~mask;
	shift = s & 0x0F;
	if (shift & 0x80)
		s = -s;
	rel_check = m & 0x40;
}
