/*    mg.c
 *
 *    Copyright (c) 1991-1999, Larry Wall
 *
 *    You may distribute under the terms of either the GNU General Public
 *    License or the Artistic License, as specified in the README file.
 *
 */

/*
 * "Sam sat on the ground and put his head in his hands.  'I wish I had never
 * come here, and I don't want to see no more magic,' he said, and fell silent."
 */

#include "EXTERN.h"
#define PERL_IN_MG_C
#include "perl.h"

/* XXX If this causes problems, set i_unistd=undef in the hint file.  */
#ifdef I_UNISTD
# include <unistd.h>
#endif

#if defined(HAS_GETGROUPS) || defined(HAS_SETGROUPS)
#  ifndef NGROUPS
#    define NGROUPS 32
#  endif
#endif

#ifdef PERL_OBJECT
#  define VTBL            this->*vtbl
#else
#  define VTBL			*vtbl
#endif

/*
 * Use the "DESTRUCTOR" scope cleanup to reinstate magic.
 */

struct magic_state {
    SV* mgs_sv;
    U32 mgs_flags;
    I32 mgs_ss_ix;
};
/* MGS is typedef'ed to struct magic_state in perl.h */

STATIC void
S_save_magic(pTHX_ I32 mgs_ix, SV *sv)
{
    dTHR;
    MGS* mgs;
    assert(SvMAGICAL(sv));

    SAVEDESTRUCTOR(S_restore_magic, (void*)mgs_ix);

    mgs = SSPTR(mgs_ix, MGS*);
    mgs->mgs_sv = sv;
    mgs->mgs_flags = SvMAGICAL(sv) | SvREADONLY(sv);
    mgs->mgs_ss_ix = PL_savestack_ix;   /* points after the saved destructor */

    SvMAGICAL_off(sv);
    SvREADONLY_off(sv);
    SvFLAGS(sv) |= (SvFLAGS(sv) & (SVp_IOK|SVp_NOK|SVp_POK)) >> PRIVSHIFT;
}

STATIC void
S_restore_magic(pTHX_ void *p)
{
    dTHR;
    MGS* mgs = SSPTR((I32)p, MGS*);
    SV* sv = mgs->mgs_sv;

    if (!sv)
        return;

    if (SvTYPE(sv) >= SVt_PVMG && SvMAGIC(sv))
    {
	if (mgs->mgs_flags)
	    SvFLAGS(sv) |= mgs->mgs_flags;
	else
	    mg_magical(sv);
	if (SvGMAGICAL(sv))
	    SvFLAGS(sv) &= ~(SVf_IOK|SVf_NOK|SVf_POK);
    }

    mgs->mgs_sv = NULL;  /* mark the MGS structure as restored */

    /* If we're still on top of the stack, pop us off.  (That condition
     * will be satisfied if restore_magic was called explicitly, but *not*
     * if it's being called via leave_scope.)
     * The reason for doing this is that otherwise, things like sv_2cv()
     * may leave alloc gunk on the savestack, and some code
     * (e.g. sighandler) doesn't expect that...
     */
    if (PL_savestack_ix == mgs->mgs_ss_ix)
    {
	I32 popval = SSPOPINT;
        assert(popval == SAVEt_DESTRUCTOR);
        PL_savestack_ix -= 2;
	popval = SSPOPINT;
        assert(popval == SAVEt_ALLOC);
	popval = SSPOPINT;
        PL_savestack_ix -= popval;
    }

}

void
Perl_mg_magical(pTHX_ SV *sv)
{
    MAGIC* mg;
    for (mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
	MGVTBL* vtbl = mg->mg_virtual;
	if (vtbl) {
	    if ((vtbl->svt_get != NULL) && !(mg->mg_flags & MGf_GSKIP))
		SvGMAGICAL_on(sv);
	    if (vtbl->svt_set)
		SvSMAGICAL_on(sv);
	    if (!(SvFLAGS(sv) & (SVs_GMG|SVs_SMG)) || (vtbl->svt_clear != NULL))
		SvRMAGICAL_on(sv);
	}
    }
}

int
Perl_mg_get(pTHX_ SV *sv)
{
    dTHR;
    I32 mgs_ix;
    MAGIC* mg;
    MAGIC** mgp;
    int mgp_valid = 0;

    mgs_ix = SSNEW(sizeof(MGS));
    save_magic(mgs_ix, sv);

    mgp = &SvMAGIC(sv);
    while ((mg = *mgp) != 0) {
	MGVTBL* vtbl = mg->mg_virtual;
	if (!(mg->mg_flags & MGf_GSKIP) && vtbl && (vtbl->svt_get != NULL)) {
	    (VTBL->svt_get)(aTHX_ sv, mg);
	    /* Ignore this magic if it's been deleted */
	    if ((mg == (mgp_valid ? *mgp : SvMAGIC(sv))) &&
		  (mg->mg_flags & MGf_GSKIP))
		(SSPTR(mgs_ix, MGS*))->mgs_flags = 0;
	}
	/* Advance to next magic (complicated by possible deletion) */
	if (mg == (mgp_valid ? *mgp : SvMAGIC(sv))) {
	    mgp = &mg->mg_moremagic;
	    mgp_valid = 1;
	}
	else
	    mgp = &SvMAGIC(sv);	/* Re-establish pointer after sv_upgrade */
    }

    restore_magic((void*)mgs_ix);
    return 0;
}

int
Perl_mg_set(pTHX_ SV *sv)
{
    dTHR;
    I32 mgs_ix;
    MAGIC* mg;
    MAGIC* nextmg;

    mgs_ix = SSNEW(sizeof(MGS));
    save_magic(mgs_ix, sv);

    for (mg = SvMAGIC(sv); mg; mg = nextmg) {
	MGVTBL* vtbl = mg->mg_virtual;
	nextmg = mg->mg_moremagic;	/* it may delete itself */
	if (mg->mg_flags & MGf_GSKIP) {
	    mg->mg_flags &= ~MGf_GSKIP;	/* setting requires another read */
	    (SSPTR(mgs_ix, MGS*))->mgs_flags = 0;
	}
	if (vtbl && (vtbl->svt_set != NULL))
	    (VTBL->svt_set)(aTHX_ sv, mg);
    }

    restore_magic((void*)mgs_ix);
    return 0;
}

U32
Perl_mg_length(pTHX_ SV *sv)
{
    MAGIC* mg;
    char *junk;
    STRLEN len;

    for (mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
	MGVTBL* vtbl = mg->mg_virtual;
	if (vtbl && (vtbl->svt_len != NULL)) {
            I32 mgs_ix;

	    mgs_ix = SSNEW(sizeof(MGS));
	    save_magic(mgs_ix, sv);
	    /* omit MGf_GSKIP -- not changed here */
	    len = (VTBL->svt_len)(aTHX_ sv, mg);
	    restore_magic((void*)mgs_ix);
	    return len;
	}
    }

    junk = SvPV(sv, len);
    return len;
}

I32
Perl_mg_size(pTHX_ SV *sv)
{
    MAGIC* mg;
    I32 len;
    
    for (mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
	MGVTBL* vtbl = mg->mg_virtual;
	if (vtbl && (vtbl->svt_len != NULL)) {
            I32 mgs_ix;

	    mgs_ix = SSNEW(sizeof(MGS));
	    save_magic(mgs_ix, sv);
	    /* omit MGf_GSKIP -- not changed here */
	    len = (VTBL->svt_len)(aTHX_ sv, mg);
	    restore_magic((void*)mgs_ix);
	    return len;
	}
    }

    switch(SvTYPE(sv)) {
	case SVt_PVAV:
	    len = AvFILLp((AV *) sv); /* Fallback to non-tied array */
	    return len;
	case SVt_PVHV:
	    /* FIXME */
	default:
	    Perl_croak(aTHX_ "Size magic not implemented");
	    break;
    }
    return 0;
}

int
Perl_mg_clear(pTHX_ SV *sv)
{
    I32 mgs_ix;
    MAGIC* mg;

    mgs_ix = SSNEW(sizeof(MGS));
    save_magic(mgs_ix, sv);

    for (mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
	MGVTBL* vtbl = mg->mg_virtual;
	/* omit GSKIP -- never set here */
	
	if (vtbl && (vtbl->svt_clear != NULL))
	    (VTBL->svt_clear)(aTHX_ sv, mg);
    }

    restore_magic((void*)mgs_ix);
    return 0;
}

MAGIC*
Perl_mg_find(pTHX_ SV *sv, int type)
{
    MAGIC* mg;
    for (mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
	if (mg->mg_type == type)
	    return mg;
    }
    return 0;
}

int
Perl_mg_copy(pTHX_ SV *sv, SV *nsv, const char *key, I32 klen)
{
    int count = 0;
    MAGIC* mg;
    for (mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
	if (isUPPER(mg->mg_type)) {
	    sv_magic(nsv,
		     mg->mg_type == 'P' ? SvTIED_obj(sv, mg) : mg->mg_obj,
		     toLOWER(mg->mg_type), key, klen);
	    count++;
	}
    }
    return count;
}

int
Perl_mg_free(pTHX_ SV *sv)
{
    MAGIC* mg;
    MAGIC* moremagic;
    for (mg = SvMAGIC(sv); mg; mg = moremagic) {
	MGVTBL* vtbl = mg->mg_virtual;
	moremagic = mg->mg_moremagic;
	if (vtbl && (vtbl->svt_free != NULL))
	    (VTBL->svt_free)(aTHX_ sv, mg);
	if (mg->mg_ptr && mg->mg_type != 'g')
	    if (mg->mg_len >= 0)
		Safefree(mg->mg_ptr);
	    else if (mg->mg_len == HEf_SVKEY)
		SvREFCNT_dec((SV*)mg->mg_ptr);
	if (mg->mg_flags & MGf_REFCOUNTED)
	    SvREFCNT_dec(mg->mg_obj);
	Safefree(mg);
    }
    SvMAGIC(sv) = 0;
    return 0;
}

#if !defined(NSIG) || defined(M_UNIX) || defined(M_XENIX)
#include <signal.h>
#endif

U32
Perl_magic_regdata_cnt(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    register char *s;
    register I32 i;
    register REGEXP *rx;
    char *t;

    if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	if (mg->mg_obj)		/* @+ */
	    return rx->nparens;
	else			/* @- */
	    return rx->lastparen;
    }
    
    return (U32)-1;
}

int
Perl_magic_regdatum_get(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    register I32 paren;
    register I32 s;
    register I32 i;
    register REGEXP *rx;
    I32 t;

    if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	paren = mg->mg_len;
	if (paren < 0)
	    return 0;
	if (paren <= rx->nparens &&
	    (s = rx->startp[paren]) != -1 &&
	    (t = rx->endp[paren]) != -1)
	    {
		if (mg->mg_obj)		/* @+ */
		    i = t;
		else			/* @- */
		    i = s;
		sv_setiv(sv,i);
	    }
    }
    return 0;
}

U32
Perl_magic_len(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    register I32 paren;
    register char *s;
    register I32 i;
    register REGEXP *rx;
    char *t;

    switch (*mg->mg_ptr) {
    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9': case '&':
	if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	    I32 s1, t1;

	    paren = atoi(mg->mg_ptr);
	  getparen:
	    if (paren <= rx->nparens &&
		(s1 = rx->startp[paren]) != -1 &&
		(t1 = rx->endp[paren]) != -1)
	    {
		i = t1 - s1;
		if (i >= 0)
		    return i;
	    }
	}
	return 0;
    case '+':
	if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	    paren = rx->lastparen;
	    if (paren)
		goto getparen;
	}
	return 0;
    case '`':
	if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	    if (rx->startp[0] != -1) {
		i = rx->startp[0];
		if (i >= 0)
		    return i;
	    }
	}
	return 0;
    case '\'':
	if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	    if (rx->endp[0] != -1) {
		i = rx->sublen - rx->endp[0];
		if (i >= 0)
		    return i;
	    }
	}
	return 0;
    case ',':
	return (STRLEN)PL_ofslen;
    case '\\':
	return (STRLEN)PL_orslen;
    }
    magic_get(sv,mg);
    if (!SvPOK(sv) && SvNIOK(sv)) {
	STRLEN n_a;
	sv_2pv(sv, &n_a);
    }
    if (SvPOK(sv))
	return SvCUR(sv);
    return 0;
}

#if 0
static char * 
printW(SV *sv)
{
#if 1
    return "" ;

#else
    int i ;
    static char buffer[50] ;
    char buf1[20] ;
    char * p ;


    sprintf(buffer, "Buffer %d, Length = %d - ", sv, SvCUR(sv)) ;
    p = SvPVX(sv) ;
    for (i = 0; i < SvCUR(sv) ; ++ i) {
        sprintf (buf1, " %x [%x]", (p+i), *(p+i)) ;
	strcat(buffer, buf1) ;
    } 

    return buffer ;

#endif
}
#endif

int
Perl_magic_get(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    register I32 paren;
    register char *s;
    register I32 i;
    register REGEXP *rx;
    char *t;

    switch (*mg->mg_ptr) {
    case '\001':		/* ^A */
	sv_setsv(sv, PL_bodytarget);
	break;
    case '\002':		/* ^B */
	/* printf("magic_get $^B: ") ; */
	if (PL_curcop->cop_warnings == WARN_NONE)
	    /* printf("WARN_NONE\n"), */
	    sv_setpvn(sv, WARN_NONEstring, WARNsize) ;
        else if (PL_curcop->cop_warnings == WARN_ALL)
	    /* printf("WARN_ALL\n"), */
	    sv_setpvn(sv, WARN_ALLstring, WARNsize) ;
        else 
	    /* printf("some %s\n", printW(PL_curcop->cop_warnings)), */
	    sv_setsv(sv, PL_curcop->cop_warnings);
	break;
    case '\003':		/* ^C */
	sv_setiv(sv, (IV)PL_minus_c);
	break;

    case '\004':		/* ^D */
	sv_setiv(sv, (IV)(PL_debug & 32767));
	break;
    case '\005':  /* ^E */
#ifdef VMS
	{
#	    include <descrip.h>
#	    include <starlet.h>
	    char msg[255];
	    $DESCRIPTOR(msgdsc,msg);
	    sv_setnv(sv,(double) vaxc$errno);
	    if (sys$getmsg(vaxc$errno,&msgdsc.dsc$w_length,&msgdsc,0,0) & 1)
		sv_setpvn(sv,msgdsc.dsc$a_pointer,msgdsc.dsc$w_length);
	    else
		sv_setpv(sv,"");
	}
#else
#ifdef OS2
	if (!(_emx_env & 0x200)) {	/* Under DOS */
	    sv_setnv(sv, (double)errno);
	    sv_setpv(sv, errno ? Strerror(errno) : "");
	} else {
	    if (errno != errno_isOS2) {
		int tmp = _syserrno();
		if (tmp)	/* 2nd call to _syserrno() makes it 0 */
		    Perl_rc = tmp;
	    }
	    sv_setnv(sv, (double)Perl_rc);
	    sv_setpv(sv, os2error(Perl_rc));
	}
#else
#ifdef WIN32
	{
	    DWORD dwErr = GetLastError();
	    sv_setnv(sv, (double)dwErr);
	    if (dwErr)
	    {
		PerlProc_GetOSError(sv, dwErr);
	    }
	    else
		sv_setpv(sv, "");
	    SetLastError(dwErr);
	}
#else
	sv_setnv(sv, (double)errno);
	sv_setpv(sv, errno ? Strerror(errno) : "");
#endif
#endif
#endif
	SvNOK_on(sv);	/* what a wonderful hack! */
	break;
    case '\006':		/* ^F */
	sv_setiv(sv, (IV)PL_maxsysfd);
	break;
    case '\010':		/* ^H */
	sv_setiv(sv, (IV)PL_hints);
	break;
    case '\011':		/* ^I */ /* NOT \t in EBCDIC */
	if (PL_inplace)
	    sv_setpv(sv, PL_inplace);
	else
	    sv_setsv(sv, &PL_sv_undef);
	break;
    case '\017':		/* ^O */
	sv_setpv(sv, PL_osname);
	break;
    case '\020':		/* ^P */
	sv_setiv(sv, (IV)PL_perldb);
	break;
    case '\023':		/* ^S */
	{
	    dTHR;
	    if (PL_lex_state != LEX_NOTPARSING)
		SvOK_off(sv);
	    else if (PL_in_eval)
		sv_setiv(sv, 1);
	    else
		sv_setiv(sv, 0);
	}
	break;
    case '\024':		/* ^T */
#ifdef BIG_TIME
 	sv_setnv(sv, PL_basetime);
#else
	sv_setiv(sv, (IV)PL_basetime);
#endif
	break;
    case '\027':		/* ^W */
	sv_setiv(sv, (IV)((PL_dowarn & G_WARN_ON) == G_WARN_ON));
	break;
    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9': case '&':
	if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	    I32 s1, t1;

	    /*
	     * Pre-threads, this was paren = atoi(GvENAME((GV*)mg->mg_obj));
	     * XXX Does the new way break anything?
	     */
	    paren = atoi(mg->mg_ptr);
	  getparen:
	    if (paren <= rx->nparens &&
		(s1 = rx->startp[paren]) != -1 &&
		(t1 = rx->endp[paren]) != -1)
	    {
		i = t1 - s1;
		s = rx->subbeg + s1;
	      getrx:
		if (i >= 0) {
		    bool was_tainted;
		    if (PL_tainting) {
			was_tainted = PL_tainted;
			PL_tainted = FALSE;
		    }
		    sv_setpvn(sv, s, i);
		    if (PL_tainting)
			PL_tainted = (was_tainted || RX_MATCH_TAINTED(rx));
		    break;
		}
	    }
	}
	sv_setsv(sv,&PL_sv_undef);
	break;
    case '+':
	if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	    paren = rx->lastparen;
	    if (paren)
		goto getparen;
	}
	sv_setsv(sv,&PL_sv_undef);
	break;
    case '`':
	if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	    if ((s = rx->subbeg) && rx->startp[0] != -1) {
		i = rx->startp[0];
		goto getrx;
	    }
	}
	sv_setsv(sv,&PL_sv_undef);
	break;
    case '\'':
	if (PL_curpm && (rx = PL_curpm->op_pmregexp)) {
	    if (rx->subbeg && rx->endp[0] != -1) {
		s = rx->subbeg + rx->endp[0];
		i = rx->sublen - rx->endp[0];
		goto getrx;
	    }
	}
	sv_setsv(sv,&PL_sv_undef);
	break;
    case '.':
#ifndef lint
	if (GvIO(PL_last_in_gv)) {
	    sv_setiv(sv, (IV)IoLINES(GvIO(PL_last_in_gv)));
	}
#endif
	break;
    case '?':
	{
	    sv_setiv(sv, (IV)STATUS_CURRENT);
#ifdef COMPLEX_STATUS
	    LvTARGOFF(sv) = PL_statusvalue;
	    LvTARGLEN(sv) = PL_statusvalue_vms;
#endif
	}
	break;
    case '^':
	s = IoTOP_NAME(GvIOp(PL_defoutgv));
	if (s)
	    sv_setpv(sv,s);
	else {
	    sv_setpv(sv,GvENAME(PL_defoutgv));
	    sv_catpv(sv,"_TOP");
	}
	break;
    case '~':
	s = IoFMT_NAME(GvIOp(PL_defoutgv));
	if (!s)
	    s = GvENAME(PL_defoutgv);
	sv_setpv(sv,s);
	break;
#ifndef lint
    case '=':
	sv_setiv(sv, (IV)IoPAGE_LEN(GvIOp(PL_defoutgv)));
	break;
    case '-':
	sv_setiv(sv, (IV)IoLINES_LEFT(GvIOp(PL_defoutgv)));
	break;
    case '%':
	sv_setiv(sv, (IV)IoPAGE(GvIOp(PL_defoutgv)));
	break;
#endif
    case ':':
	break;
    case '/':
	break;
    case '[':
	WITH_THR(sv_setiv(sv, (IV)PL_curcop->cop_arybase));
	break;
    case '|':
	sv_setiv(sv, (IV)(IoFLAGS(GvIOp(PL_defoutgv)) & IOf_FLUSH) != 0 );
	break;
    case ',':
	sv_setpvn(sv,PL_ofs,PL_ofslen);
	break;
    case '\\':
	sv_setpvn(sv,PL_ors,PL_orslen);
	break;
    case '#':
	sv_setpv(sv,PL_ofmt);
	break;
    case '!':
#ifdef VMS
	sv_setnv(sv, (double)((errno == EVMSERR) ? vaxc$errno : errno));
	sv_setpv(sv, errno ? Strerror(errno) : "");
#else
	{
	int saveerrno = errno;
	sv_setnv(sv, (double)errno);
#ifdef OS2
	if (errno == errno_isOS2) sv_setpv(sv, os2error(Perl_rc));
	else
#endif
	sv_setpv(sv, errno ? Strerror(errno) : "");
	errno = saveerrno;
	}
#endif
	SvNOK_on(sv);	/* what a wonderful hack! */
	break;
    case '<':
	sv_setiv(sv, (IV)PL_uid);
	break;
    case '>':
	sv_setiv(sv, (IV)PL_euid);
	break;
    case '(':
	sv_setiv(sv, (IV)PL_gid);
	Perl_sv_setpvf(aTHX_ sv, "%Vd", (IV)PL_gid);
	goto add_groups;
    case ')':
	sv_setiv(sv, (IV)PL_egid);
	Perl_sv_setpvf(aTHX_ sv, "%Vd", (IV)PL_egid);
      add_groups:
#ifdef HAS_GETGROUPS
	{
	    Groups_t gary[NGROUPS];
	    i = getgroups(NGROUPS,gary);
	    while (--i >= 0)
		Perl_sv_catpvf(aTHX_ sv, " %Vd", (IV)gary[i]);
	}
#endif
	SvIOK_on(sv);	/* what a wonderful hack! */
	break;
    case '*':
	break;
    case '0':
	break;
#ifdef USE_THREADS
    case '@':
	sv_setsv(sv, thr->errsv);
	break;
#endif /* USE_THREADS */
    }
    return 0;
}

int
Perl_magic_getuvar(pTHX_ SV *sv, MAGIC *mg)
{
    struct ufuncs *uf = (struct ufuncs *)mg->mg_ptr;

    if (uf && uf->uf_val)
	(*uf->uf_val)(uf->uf_index, sv);
    return 0;
}

int
Perl_magic_setenv(pTHX_ SV *sv, MAGIC *mg)
{
    register char *s;
    char *ptr;
    STRLEN len, klen;
    I32 i;

    s = SvPV(sv,len);
    ptr = MgPV(mg,klen);
    my_setenv(ptr, s);

#ifdef DYNAMIC_ENV_FETCH
     /* We just undefd an environment var.  Is a replacement */
     /* waiting in the wings? */
    if (!len) {
	SV **valp;
	if ((valp = hv_fetch(GvHVn(PL_envgv), ptr, klen, FALSE)))
	    s = SvPV(*valp, len);
    }
#endif

#if !defined(OS2) && !defined(AMIGAOS) && !defined(WIN32) && !defined(MSDOS)
			    /* And you'll never guess what the dog had */
			    /*   in its mouth... */
    if (PL_tainting) {
	MgTAINTEDDIR_off(mg);
#ifdef VMS
	if (s && klen == 8 && strEQ(ptr, "DCL$PATH")) {
	    char pathbuf[256], eltbuf[256], *cp, *elt = s;
	    struct stat sbuf;
	    int i = 0, j = 0;

	    do {          /* DCL$PATH may be a search list */
		while (1) {   /* as may dev portion of any element */
		    if ( ((cp = strchr(elt,'[')) || (cp = strchr(elt,'<'))) ) {
			if ( *(cp+1) == '.' || *(cp+1) == '-' ||
			     cando_by_name(S_IWUSR,0,elt) ) {
			    MgTAINTEDDIR_on(mg);
			    return 0;
			}
		    }
		    if ((cp = strchr(elt, ':')) != Nullch)
			*cp = '\0';
		    if (my_trnlnm(elt, eltbuf, j++))
			elt = eltbuf;
		    else
			break;
		}
		j = 0;
	    } while (my_trnlnm(s, pathbuf, i++) && (elt = pathbuf));
	}
#endif /* VMS */
	if (s && klen == 4 && strEQ(ptr,"PATH")) {
	    char *strend = s + len;

	    while (s < strend) {
		char tmpbuf[256];
		struct stat st;
		s = delimcpy(tmpbuf, tmpbuf + sizeof tmpbuf,
			     s, strend, ':', &i);
		s++;
		if (i >= sizeof tmpbuf   /* too long -- assume the worst */
		      || *tmpbuf != '/'
		      || (PerlLIO_stat(tmpbuf, &st) == 0 && (st.st_mode & 2)) ) {
		    MgTAINTEDDIR_on(mg);
		    return 0;
		}
	    }
	}
    }
#endif /* neither OS2 nor AMIGAOS nor WIN32 nor MSDOS */

    return 0;
}

int
Perl_magic_clearenv(pTHX_ SV *sv, MAGIC *mg)
{
    STRLEN n_a;
    my_setenv(MgPV(mg,n_a),Nullch);
    return 0;
}

int
Perl_magic_set_all_env(pTHX_ SV *sv, MAGIC *mg)
{
#if defined(VMS)
    Perl_die(aTHX_ "Can't make list assignment to %%ENV on this system");
#else
    dTHR;
    if (PL_localizing) {
	HE* entry;
	STRLEN n_a;
	magic_clear_all_env(sv,mg);
	hv_iterinit((HV*)sv);
	while (entry = hv_iternext((HV*)sv)) {
	    I32 keylen;
	    my_setenv(hv_iterkey(entry, &keylen),
		      SvPV(hv_iterval((HV*)sv, entry), n_a));
	}
    }
#endif
    return 0;
}

int
Perl_magic_clear_all_env(pTHX_ SV *sv, MAGIC *mg)
{
#if defined(VMS)
    Perl_die(aTHX_ "Can't make list assignment to %%ENV on this system");
#else
#  ifdef WIN32
    char *envv = GetEnvironmentStrings();
    char *cur = envv;
    STRLEN len;
    while (*cur) {
	char *end = strchr(cur,'=');
	if (end && end != cur) {
	    *end = '\0';
	    my_setenv(cur,Nullch);
	    *end = '=';
	    cur = end + strlen(end+1)+2;
	}
	else if ((len = strlen(cur)))
	    cur += len+1;
    }
    FreeEnvironmentStrings(envv);
#  else
#    ifndef PERL_USE_SAFE_PUTENV
    I32 i;

    if (environ == PL_origenviron)
	environ = (char**)safesysmalloc(sizeof(char*));
    else
	for (i = 0; environ[i]; i++)
	    safesysfree(environ[i]);
#    endif /* PERL_USE_SAFE_PUTENV */

    environ[0] = Nullch;

#  endif /* WIN32 */
#endif /* VMS */
    return 0;
}

int
Perl_magic_getsig(pTHX_ SV *sv, MAGIC *mg)
{
    I32 i;
    STRLEN n_a;
    /* Are we fetching a signal entry? */
    i = whichsig(MgPV(mg,n_a));
    if (i) {
    	if(PL_psig_ptr[i])
    	    sv_setsv(sv,PL_psig_ptr[i]);
    	else {
    	    Sighandler_t sigstate = rsignal_state(i);

    	    /* cache state so we don't fetch it again */
    	    if(sigstate == SIG_IGN)
    	    	sv_setpv(sv,"IGNORE");
    	    else
    	    	sv_setsv(sv,&PL_sv_undef);
    	    PL_psig_ptr[i] = SvREFCNT_inc(sv);
    	    SvTEMP_off(sv);
    	}
    }
    return 0;
}
int
Perl_magic_clearsig(pTHX_ SV *sv, MAGIC *mg)
{
    I32 i;
    STRLEN n_a;
    /* Are we clearing a signal entry? */
    i = whichsig(MgPV(mg,n_a));
    if (i) {
    	if(PL_psig_ptr[i]) {
    	    SvREFCNT_dec(PL_psig_ptr[i]);
    	    PL_psig_ptr[i]=0;
    	}
    	if(PL_psig_name[i]) {
    	    SvREFCNT_dec(PL_psig_name[i]);
    	    PL_psig_name[i]=0;
    	}
    }
    return 0;
}

int
Perl_magic_setsig(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    register char *s;
    I32 i;
    SV** svp;
    STRLEN len;

    s = MgPV(mg,len);
    if (*s == '_') {
	if (strEQ(s,"__DIE__"))
	    svp = &PL_diehook;
	else if (strEQ(s,"__WARN__"))
	    svp = &PL_warnhook;
	else if (strEQ(s,"__PARSE__"))
	    svp = &PL_parsehook;
	else
	    Perl_croak(aTHX_ "No such hook: %s", s);
	i = 0;
	if (*svp) {
	    SvREFCNT_dec(*svp);
	    *svp = 0;
	}
    }
    else {
	i = whichsig(s);	/* ...no, a brick */
	if (!i) {
	    if (ckWARN(WARN_SIGNAL) || strEQ(s,"ALARM"))
		Perl_warner(aTHX_ WARN_SIGNAL, "No such signal: SIG%s", s);
	    return 0;
	}
	SvREFCNT_dec(PL_psig_name[i]);
	SvREFCNT_dec(PL_psig_ptr[i]);
	PL_psig_ptr[i] = SvREFCNT_inc(sv);
	SvTEMP_off(sv); /* Make sure it doesn't go away on us */
	PL_psig_name[i] = newSVpvn(s, len);
	SvREADONLY_on(PL_psig_name[i]);
    }
    if (SvTYPE(sv) == SVt_PVGV || SvROK(sv)) {
	if (i)
	    (void)rsignal(i, PL_sighandlerp);
	else
	    *svp = SvREFCNT_inc(sv);
	return 0;
    }
    s = SvPV_force(sv,len);
    if (strEQ(s,"IGNORE")) {
	if (i)
	    (void)rsignal(i, SIG_IGN);
	else
	    *svp = 0;
    }
    else if (strEQ(s,"DEFAULT") || !*s) {
	if (i)
	    (void)rsignal(i, SIG_DFL);
	else
	    *svp = 0;
    }
    else {
	/*
	 * We should warn if HINT_STRICT_REFS, but without
	 * access to a known hint bit in a known OP, we can't
	 * tell whether HINT_STRICT_REFS is in force or not.
	 */
	if (!strchr(s,':') && !strchr(s,'\''))
	    sv_insert(sv, 0, 0, "main::", 6);
	if (i)
	    (void)rsignal(i, PL_sighandlerp);
	else
	    *svp = SvREFCNT_inc(sv);
    }
    return 0;
}

int
Perl_magic_setisa(pTHX_ SV *sv, MAGIC *mg)
{
    PL_sub_generation++;
    return 0;
}

int
Perl_magic_setamagic(pTHX_ SV *sv, MAGIC *mg)
{
    /* HV_badAMAGIC_on(Sv_STASH(sv)); */
    PL_amagic_generation++;

    return 0;
}

int
Perl_magic_getnkeys(pTHX_ SV *sv, MAGIC *mg)
{
    HV *hv = (HV*)LvTARG(sv);
    HE *entry;
    I32 i = 0;

    if (hv) {
	(void) hv_iterinit(hv);
	if (! SvTIED_mg((SV*)hv, 'P'))
	    i = HvKEYS(hv);
	else {
	    /*SUPPRESS 560*/
	    while (entry = hv_iternext(hv)) {
		i++;
	    }
	}
    }

    sv_setiv(sv, (IV)i);
    return 0;
}

int
Perl_magic_setnkeys(pTHX_ SV *sv, MAGIC *mg)
{
    if (LvTARG(sv)) {
	hv_ksplit((HV*)LvTARG(sv), SvIV(sv));
    }
    return 0;
}          

/* caller is responsible for stack switching/cleanup */
STATIC int
S_magic_methcall(pTHX_ SV *sv, MAGIC *mg, char *meth, I32 flags, int n, SV *val)
{
    dSP;

    PUSHMARK(SP);
    EXTEND(SP, n);
    PUSHs(SvTIED_obj(sv, mg));
    if (n > 1) { 
	if (mg->mg_ptr) {
	    if (mg->mg_len >= 0)
		PUSHs(sv_2mortal(newSVpvn(mg->mg_ptr, mg->mg_len)));
	    else if (mg->mg_len == HEf_SVKEY)
		PUSHs((SV*)mg->mg_ptr);
	}
	else if (mg->mg_type == 'p') {
	    PUSHs(sv_2mortal(newSViv(mg->mg_len)));
	}
    }
    if (n > 2) {
	PUSHs(val);
    }
    PUTBACK;

    return call_method(meth, flags);
}

STATIC int
S_magic_methpack(pTHX_ SV *sv, MAGIC *mg, char *meth)
{
    dSP;

    ENTER;
    SAVETMPS;
    PUSHSTACKi(PERLSI_MAGIC);

    if (magic_methcall(sv, mg, meth, G_SCALAR, 2, NULL)) {
	sv_setsv(sv, *PL_stack_sp--);
    }

    POPSTACK;
    FREETMPS;
    LEAVE;
    return 0;
}

int
Perl_magic_getpack(pTHX_ SV *sv, MAGIC *mg)
{
    magic_methpack(sv,mg,"FETCH");
    if (mg->mg_ptr)
	mg->mg_flags |= MGf_GSKIP;
    return 0;
}

int
Perl_magic_setpack(pTHX_ SV *sv, MAGIC *mg)
{
    dSP;
    ENTER;
    PUSHSTACKi(PERLSI_MAGIC);
    magic_methcall(sv, mg, "STORE", G_SCALAR|G_DISCARD, 3, sv);
    POPSTACK;
    LEAVE;
    return 0;
}

int
Perl_magic_clearpack(pTHX_ SV *sv, MAGIC *mg)
{
    return magic_methpack(sv,mg,"DELETE");
}


U32
Perl_magic_sizepack(pTHX_ SV *sv, MAGIC *mg)
{         
    dSP;
    U32 retval = 0;

    ENTER;
    SAVETMPS;
    PUSHSTACKi(PERLSI_MAGIC);
    if (magic_methcall(sv, mg, "FETCHSIZE", G_SCALAR, 2, NULL)) {
	sv = *PL_stack_sp--;
	retval = (U32) SvIV(sv)-1;
    }
    POPSTACK;
    FREETMPS;
    LEAVE;
    return retval;
}

int
Perl_magic_wipepack(pTHX_ SV *sv, MAGIC *mg)
{
    dSP;

    ENTER;
    PUSHSTACKi(PERLSI_MAGIC);
    PUSHMARK(SP);
    XPUSHs(SvTIED_obj(sv, mg));
    PUTBACK;
    call_method("CLEAR", G_SCALAR|G_DISCARD);
    POPSTACK;
    LEAVE;
    return 0;
}

int
Perl_magic_nextpack(pTHX_ SV *sv, MAGIC *mg, SV *key)
{
    dSP;
    char *meth = SvOK(key) ? "NEXTKEY" : "FIRSTKEY";

    ENTER;
    SAVETMPS;
    PUSHSTACKi(PERLSI_MAGIC);
    PUSHMARK(SP);
    EXTEND(SP, 2);
    PUSHs(SvTIED_obj(sv, mg));
    if (SvOK(key))
	PUSHs(key);
    PUTBACK;

    if (call_method(meth, G_SCALAR))
	sv_setsv(key, *PL_stack_sp--);

    POPSTACK;
    FREETMPS;
    LEAVE;
    return 0;
}

int
Perl_magic_existspack(pTHX_ SV *sv, MAGIC *mg)
{
    return magic_methpack(sv,mg,"EXISTS");
} 

int
Perl_magic_setdbline(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    OP *o;
    I32 i;
    GV* gv;
    SV** svp;
    STRLEN n_a;

    gv = PL_DBline;
    i = SvTRUE(sv);
    svp = av_fetch(GvAV(gv),
		     atoi(MgPV(mg,n_a)), FALSE);
    if (svp && SvIOKp(*svp) && (o = (OP*)SvSTASH(*svp)))
	o->op_private = i;
    else
	Perl_warn(aTHX_ "Can't break at that line\n");
    return 0;
}

int
Perl_magic_getarylen(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    sv_setiv(sv, AvFILL((AV*)mg->mg_obj) + PL_curcop->cop_arybase);
    return 0;
}

int
Perl_magic_setarylen(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    av_fill((AV*)mg->mg_obj, SvIV(sv) - PL_curcop->cop_arybase);
    return 0;
}

int
Perl_magic_getpos(pTHX_ SV *sv, MAGIC *mg)
{
    SV* lsv = LvTARG(sv);
    
    if (SvTYPE(lsv) >= SVt_PVMG && SvMAGIC(lsv)) {
	mg = mg_find(lsv, 'g');
	if (mg && mg->mg_len >= 0) {
	    dTHR;
	    I32 i = mg->mg_len;
	    if (IN_UTF8)
		sv_pos_b2u(lsv, &i);
	    sv_setiv(sv, i + PL_curcop->cop_arybase);
	    return 0;
	}
    }
    (void)SvOK_off(sv);
    return 0;
}

int
Perl_magic_setpos(pTHX_ SV *sv, MAGIC *mg)
{
    SV* lsv = LvTARG(sv);
    SSize_t pos;
    STRLEN len;
    STRLEN ulen;
    dTHR;

    mg = 0;
    
    if (SvTYPE(lsv) >= SVt_PVMG && SvMAGIC(lsv))
	mg = mg_find(lsv, 'g');
    if (!mg) {
	if (!SvOK(sv))
	    return 0;
	sv_magic(lsv, (SV*)0, 'g', Nullch, 0);
	mg = mg_find(lsv, 'g');
    }
    else if (!SvOK(sv)) {
	mg->mg_len = -1;
	return 0;
    }
    len = SvPOK(lsv) ? SvCUR(lsv) : sv_len(lsv);

    pos = SvIV(sv) - PL_curcop->cop_arybase;

    if (IN_UTF8) {
	ulen = sv_len_utf8(lsv);
	if (ulen)
	    len = ulen;
	else
	    ulen = 0;
    }

    if (pos < 0) {
	pos += len;
	if (pos < 0)
	    pos = 0;
    }
    else if (pos > len)
	pos = len;

    if (ulen) {
	I32 p = pos;
	sv_pos_u2b(lsv, &p, 0);
	pos = p;
    }
	
    mg->mg_len = pos;
    mg->mg_flags &= ~MGf_MINMATCH;

    return 0;
}

int
Perl_magic_getglob(pTHX_ SV *sv, MAGIC *mg)
{
    if (SvFAKE(sv)) {			/* FAKE globs can get coerced */
	SvFAKE_off(sv);
	gv_efullname3(sv,((GV*)sv), "*");
	SvFAKE_on(sv);
    }
    else
	gv_efullname3(sv,((GV*)sv), "*");	/* a gv value, be nice */
    return 0;
}

int
Perl_magic_setglob(pTHX_ SV *sv, MAGIC *mg)
{
    register char *s;
    GV* gv;
    STRLEN n_a;

    if (!SvOK(sv))
	return 0;
    s = SvPV(sv, n_a);
    if (*s == '*' && s[1])
	s++;
    gv = gv_fetchpv(s,TRUE, SVt_PVGV);
    if (sv == (SV*)gv)
	return 0;
    if (GvGP(sv))
	gp_free((GV*)sv);
    GvGP(sv) = gp_ref(GvGP(gv));
    return 0;
}

int
Perl_magic_getsubstr(pTHX_ SV *sv, MAGIC *mg)
{
    STRLEN len;
    SV *lsv = LvTARG(sv);
    char *tmps = SvPV(lsv,len);
    I32 offs = LvTARGOFF(sv);
    I32 rem = LvTARGLEN(sv);

    if (offs > len)
	offs = len;
    if (rem + offs > len)
	rem = len - offs;
    sv_setpvn(sv, tmps + offs, (STRLEN)rem);
    return 0;
}

int
Perl_magic_setsubstr(pTHX_ SV *sv, MAGIC *mg)
{
    STRLEN len;
    char *tmps = SvPV(sv,len);
    sv_insert(LvTARG(sv),LvTARGOFF(sv),LvTARGLEN(sv), tmps, len);
    return 0;
}

int
Perl_magic_gettaint(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    TAINT_IF((mg->mg_len & 1) ||
	     (mg->mg_len & 2) && mg->mg_obj == sv);	/* kludge */
    return 0;
}

int
Perl_magic_settaint(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    if (PL_localizing) {
	if (PL_localizing == 1)
	    mg->mg_len <<= 1;
	else
	    mg->mg_len >>= 1;
    }
    else if (PL_tainted)
	mg->mg_len |= 1;
    else
	mg->mg_len &= ~1;
    return 0;
}

int
Perl_magic_getvec(pTHX_ SV *sv, MAGIC *mg)
{
    SV *lsv = LvTARG(sv);
    unsigned char *s;
    unsigned long retnum;
    STRLEN lsvlen;
    I32 len;
    I32 offset;
    I32 size;

    if (!lsv) {
	SvOK_off(sv);
	return 0;
    }
    s = (unsigned char *) SvPV(lsv, lsvlen);
    offset = LvTARGOFF(sv);
    size = LvTARGLEN(sv);
    len = (offset + size + 7) / 8;

    /* Copied from pp_vec() */

    if (len > lsvlen) {
	if (size <= 8)
	    retnum = 0;
	else {
	    offset >>= 3;
	    if (size == 16) {
		if (offset >= lsvlen)
		    retnum = 0;
		else
		    retnum = (unsigned long) s[offset] << 8;
	    }
	    else if (size == 32) {
		if (offset >= lsvlen)
		    retnum = 0;
		else if (offset + 1 >= lsvlen)
		    retnum = (unsigned long) s[offset] << 24;
		else if (offset + 2 >= lsvlen)
		    retnum = ((unsigned long) s[offset] << 24) +
			((unsigned long) s[offset + 1] << 16);
		else
		    retnum = ((unsigned long) s[offset] << 24) +
			((unsigned long) s[offset + 1] << 16) +
			(s[offset + 2] << 8);
	    }
	}
    }
    else if (size < 8)
	retnum = (s[offset >> 3] >> (offset & 7)) & ((1 << size) - 1);
    else {
	offset >>= 3;
	if (size == 8)
	    retnum = s[offset];
	else if (size == 16)
	    retnum = ((unsigned long) s[offset] << 8) + s[offset+1];
	else if (size == 32)
	    retnum = ((unsigned long) s[offset] << 24) +
		((unsigned long) s[offset + 1] << 16) +
		(s[offset + 2] << 8) + s[offset+3];
    }

    sv_setuv(sv, (UV)retnum);
    return 0;
}

int
Perl_magic_setvec(pTHX_ SV *sv, MAGIC *mg)
{
    do_vecset(sv);	/* XXX slurp this routine */
    return 0;
}

int
Perl_magic_getdefelem(pTHX_ SV *sv, MAGIC *mg)
{
    SV *targ = Nullsv;
    if (LvTARGLEN(sv)) {
	if (mg->mg_obj) {
	    SV *ahv = LvTARG(sv);
	    if (SvTYPE(ahv) == SVt_PVHV) {
		HE *he = hv_fetch_ent((HV*)ahv, mg->mg_obj, FALSE, 0);
		if (he)
		    targ = HeVAL(he);
	    }
	    else {
		SV **svp = avhv_fetch_ent((AV*)ahv, mg->mg_obj, FALSE, 0);
		if (svp)
		    targ = *svp;
	    }
	}
	else {
	    AV* av = (AV*)LvTARG(sv);
	    if ((I32)LvTARGOFF(sv) <= AvFILL(av))
		targ = AvARRAY(av)[LvTARGOFF(sv)];
	}
	if (targ && targ != &PL_sv_undef) {
	    dTHR;		/* just for SvREFCNT_dec */
	    /* somebody else defined it for us */
	    SvREFCNT_dec(LvTARG(sv));
	    LvTARG(sv) = SvREFCNT_inc(targ);
	    LvTARGLEN(sv) = 0;
	    SvREFCNT_dec(mg->mg_obj);
	    mg->mg_obj = Nullsv;
	    mg->mg_flags &= ~MGf_REFCOUNTED;
	}
    }
    else
	targ = LvTARG(sv);
    sv_setsv(sv, targ ? targ : &PL_sv_undef);
    return 0;
}

int
Perl_magic_setdefelem(pTHX_ SV *sv, MAGIC *mg)
{
    if (LvTARGLEN(sv))
	vivify_defelem(sv);
    if (LvTARG(sv)) {
	sv_setsv(LvTARG(sv), sv);
	SvSETMAGIC(LvTARG(sv));
    }
    return 0;
}

void
Perl_vivify_defelem(pTHX_ SV *sv)
{
    dTHR;			/* just for SvREFCNT_inc and SvREFCNT_dec*/
    MAGIC *mg;
    SV *value = Nullsv;

    if (!LvTARGLEN(sv) || !(mg = mg_find(sv, 'y')))
	return;
    if (mg->mg_obj) {
	SV *ahv = LvTARG(sv);
	STRLEN n_a;
	if (SvTYPE(ahv) == SVt_PVHV) {
	    HE *he = hv_fetch_ent((HV*)ahv, mg->mg_obj, TRUE, 0);
	    if (he)
		value = HeVAL(he);
	}
	else {
	    SV **svp = avhv_fetch_ent((AV*)ahv, mg->mg_obj, TRUE, 0);
	    if (svp)
		value = *svp;
	}
	if (!value || value == &PL_sv_undef)
	    Perl_croak(aTHX_ PL_no_helem, SvPV(mg->mg_obj, n_a));
    }
    else {
	AV* av = (AV*)LvTARG(sv);
	if ((I32)LvTARGLEN(sv) < 0 && (I32)LvTARGOFF(sv) > AvFILL(av))
	    LvTARG(sv) = Nullsv;	/* array can't be extended */
	else {
	    SV** svp = av_fetch(av, LvTARGOFF(sv), TRUE);
	    if (!svp || (value = *svp) == &PL_sv_undef)
		Perl_croak(aTHX_ PL_no_aelem, (I32)LvTARGOFF(sv));
	}
    }
    (void)SvREFCNT_inc(value);
    SvREFCNT_dec(LvTARG(sv));
    LvTARG(sv) = value;
    LvTARGLEN(sv) = 0;
    SvREFCNT_dec(mg->mg_obj);
    mg->mg_obj = Nullsv;
    mg->mg_flags &= ~MGf_REFCOUNTED;
}

int
Perl_magic_killbackrefs(pTHX_ SV *sv, MAGIC *mg)
{
    AV *av = (AV*)mg->mg_obj;
    SV **svp = AvARRAY(av);
    I32 i = AvFILLp(av);
    while (i >= 0) {
	if (svp[i] && svp[i] != &PL_sv_undef) {
	    if (!SvWEAKREF(svp[i]))
		Perl_croak(aTHX_ "panic: magic_killbackrefs");
	    /* XXX Should we check that it hasn't changed? */
	    SvRV(svp[i]) = 0;
	    SvOK_off(svp[i]);
	    SvWEAKREF_off(svp[i]);
	    svp[i] = &PL_sv_undef;
	}
	i--;
    }
    return 0;
}

int
Perl_magic_setmglob(pTHX_ SV *sv, MAGIC *mg)
{
    mg->mg_len = -1;
    SvSCREAM_off(sv);
    return 0;
}

int
Perl_magic_setbm(pTHX_ SV *sv, MAGIC *mg)
{
    sv_unmagic(sv, 'B');
    SvVALID_off(sv);
    return 0;
}

int
Perl_magic_setfm(pTHX_ SV *sv, MAGIC *mg)
{
    sv_unmagic(sv, 'f');
    SvCOMPILED_off(sv);
    return 0;
}

int
Perl_magic_setuvar(pTHX_ SV *sv, MAGIC *mg)
{
    struct ufuncs *uf = (struct ufuncs *)mg->mg_ptr;

    if (uf && uf->uf_set)
	(*uf->uf_set)(uf->uf_index, sv);
    return 0;
}

int
Perl_magic_freeregexp(pTHX_ SV *sv, MAGIC *mg)
{
    regexp *re = (regexp *)mg->mg_obj;
    ReREFCNT_dec(re);
    return 0;
}

#ifdef USE_LOCALE_COLLATE
int
Perl_magic_setcollxfrm(pTHX_ SV *sv, MAGIC *mg)
{
    /*
     * RenE<eacute> Descartes said "I think not."
     * and vanished with a faint plop.
     */
    if (mg->mg_ptr) {
	Safefree(mg->mg_ptr);
	mg->mg_ptr = NULL;
	mg->mg_len = -1;
    }
    return 0;
}
#endif /* USE_LOCALE_COLLATE */

int
Perl_magic_set(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    register char *s;
    I32 i;
    STRLEN len;
    switch (*mg->mg_ptr) {
    case '\001':	/* ^A */
	sv_setsv(PL_bodytarget, sv);
	break;
    case '\002':	/* ^B */
	if ( ! (PL_dowarn & G_WARN_ALL_MASK)) {
            if (memEQ(SvPVX(sv), WARN_ALLstring, WARNsize))
	        PL_compiling.cop_warnings = WARN_ALL;
	    else if (memEQ(SvPVX(sv), WARN_NONEstring, WARNsize))
	        PL_compiling.cop_warnings = WARN_NONE;
            else {
	        if (PL_compiling.cop_warnings != WARN_NONE && 
		    PL_compiling.cop_warnings != WARN_ALL)
	            sv_setsv(PL_compiling.cop_warnings, sv);
	        else
		    PL_compiling.cop_warnings = newSVsv(sv) ;
	    }
	}
	break;

    case '\003':	/* ^C */
	PL_minus_c = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	break;

    case '\004':	/* ^D */
	PL_debug = (SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv)) | 0x80000000;
	DEBUG_x(dump_all());
	break;
    case '\005':  /* ^E */
#ifdef VMS
	set_vaxc_errno(SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv));
#else
#  ifdef WIN32
	SetLastError( SvIV(sv) );
#  else
#    ifndef OS2
	/* will anyone ever use this? */
	SETERRNO(SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv), 4);
#    endif
#  endif
#endif
	break;
    case '\006':	/* ^F */
	PL_maxsysfd = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	break;
    case '\010':	/* ^H */
	PL_hints = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	break;
    case '\011':	/* ^I */ /* NOT \t in EBCDIC */
	if (PL_inplace)
	    Safefree(PL_inplace);
	if (SvOK(sv))
	    PL_inplace = savepv(SvPV(sv,len));
	else
	    PL_inplace = Nullch;
	break;
    case '\017':	/* ^O */
	if (PL_osname)
	    Safefree(PL_osname);
	if (SvOK(sv))
	    PL_osname = savepv(SvPV(sv,len));
	else
	    PL_osname = Nullch;
	break;
    case '\020':	/* ^P */
	PL_perldb = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	break;
    case '\024':	/* ^T */
#ifdef BIG_TIME
	PL_basetime = (Time_t)(SvNOK(sv) ? SvNVX(sv) : sv_2nv(sv));
#else
	PL_basetime = (Time_t)(SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv));
#endif
	break;
    case '\027':	/* ^W */
	if ( ! (PL_dowarn & G_WARN_ALL_MASK)) {
	    i = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	    PL_dowarn = (i ? G_WARN_ON : G_WARN_OFF) ;
	}
	break;
    case '.':
	if (PL_localizing) {
	    if (PL_localizing == 1)
		save_sptr((SV**)&PL_last_in_gv);
	}
	else if (SvOK(sv) && GvIO(PL_last_in_gv))
	    IoLINES(GvIOp(PL_last_in_gv)) = (long)SvIV(sv);
	break;
    case '^':
	Safefree(IoTOP_NAME(GvIOp(PL_defoutgv)));
	IoTOP_NAME(GvIOp(PL_defoutgv)) = s = savepv(SvPV(sv,len));
	IoTOP_GV(GvIOp(PL_defoutgv)) = gv_fetchpv(s,TRUE, SVt_PVIO);
	break;
    case '~':
	Safefree(IoFMT_NAME(GvIOp(PL_defoutgv)));
	IoFMT_NAME(GvIOp(PL_defoutgv)) = s = savepv(SvPV(sv,len));
	IoFMT_GV(GvIOp(PL_defoutgv)) = gv_fetchpv(s,TRUE, SVt_PVIO);
	break;
    case '=':
	IoPAGE_LEN(GvIOp(PL_defoutgv)) = (long)(SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv));
	break;
    case '-':
	IoLINES_LEFT(GvIOp(PL_defoutgv)) = (long)(SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv));
	if (IoLINES_LEFT(GvIOp(PL_defoutgv)) < 0L)
	    IoLINES_LEFT(GvIOp(PL_defoutgv)) = 0L;
	break;
    case '%':
	IoPAGE(GvIOp(PL_defoutgv)) = (long)(SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv));
	break;
    case '|':
	{
	    IO *io = GvIOp(PL_defoutgv);
	    if ((SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv)) == 0)
		IoFLAGS(io) &= ~IOf_FLUSH;
	    else {
		if (!(IoFLAGS(io) & IOf_FLUSH)) {
		    PerlIO *ofp = IoOFP(io);
		    if (ofp)
			(void)PerlIO_flush(ofp);
		    IoFLAGS(io) |= IOf_FLUSH;
		}
	    }
	}
	break;
    case '*':
	i = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	PL_multiline = (i != 0);
	break;
    case '/':
	SvREFCNT_dec(PL_nrs);
	PL_nrs = newSVsv(sv);
	SvREFCNT_dec(PL_rs);
	PL_rs = SvREFCNT_inc(PL_nrs);
	break;
    case '\\':
	if (PL_ors)
	    Safefree(PL_ors);
	if (SvOK(sv) || SvGMAGICAL(sv))
	    PL_ors = savepv(SvPV(sv,PL_orslen));
	else {
	    PL_ors = Nullch;
	    PL_orslen = 0;
	}
	break;
    case ',':
	if (PL_ofs)
	    Safefree(PL_ofs);
	PL_ofs = savepv(SvPV(sv, PL_ofslen));
	break;
    case '#':
	if (PL_ofmt)
	    Safefree(PL_ofmt);
	PL_ofmt = savepv(SvPV(sv,len));
	break;
    case '[':
	PL_compiling.cop_arybase = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	break;
    case '?':
#ifdef COMPLEX_STATUS
	if (PL_localizing == 2) {
	    PL_statusvalue = LvTARGOFF(sv);
	    PL_statusvalue_vms = LvTARGLEN(sv);
	}
	else
#endif
#ifdef VMSISH_STATUS
	if (VMSISH_STATUS)
	    STATUS_NATIVE_SET((U32)(SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv)));
	else
#endif
	    STATUS_POSIX_SET(SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv));
	break;
    case '!':
	SETERRNO(SvIOK(sv) ? SvIVX(sv) : SvOK(sv) ? sv_2iv(sv) : 0,
		 (SvIV(sv) == EVMSERR) ? 4 : vaxc$errno);
	break;
    case '<':
	PL_uid = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	if (PL_delaymagic) {
	    PL_delaymagic |= DM_RUID;
	    break;				/* don't do magic till later */
	}
#ifdef HAS_SETRUID
	(void)setruid((Uid_t)PL_uid);
#else
#ifdef HAS_SETREUID
	(void)setreuid((Uid_t)PL_uid, (Uid_t)-1);
#else
#ifdef HAS_SETRESUID
      (void)setresuid((Uid_t)PL_uid, (Uid_t)-1, (Uid_t)-1);
#else
	if (PL_uid == PL_euid)		/* special case $< = $> */
	    (void)PerlProc_setuid(PL_uid);
	else {
	    PL_uid = (I32)PerlProc_getuid();
	    Perl_croak(aTHX_ "setruid() not implemented");
	}
#endif
#endif
#endif
	PL_uid = (I32)PerlProc_getuid();
	PL_tainting |= (PL_uid && (PL_euid != PL_uid || PL_egid != PL_gid));
	break;
    case '>':
	PL_euid = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	if (PL_delaymagic) {
	    PL_delaymagic |= DM_EUID;
	    break;				/* don't do magic till later */
	}
#ifdef HAS_SETEUID
	(void)seteuid((Uid_t)PL_euid);
#else
#ifdef HAS_SETREUID
	(void)setreuid((Uid_t)-1, (Uid_t)PL_euid);
#else
#ifdef HAS_SETRESUID
	(void)setresuid((Uid_t)-1, (Uid_t)PL_euid, (Uid_t)-1);
#else
	if (PL_euid == PL_uid)		/* special case $> = $< */
	    PerlProc_setuid(PL_euid);
	else {
	    PL_euid = (I32)PerlProc_geteuid();
	    Perl_croak(aTHX_ "seteuid() not implemented");
	}
#endif
#endif
#endif
	PL_euid = (I32)PerlProc_geteuid();
	PL_tainting |= (PL_uid && (PL_euid != PL_uid || PL_egid != PL_gid));
	break;
    case '(':
	PL_gid = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
	if (PL_delaymagic) {
	    PL_delaymagic |= DM_RGID;
	    break;				/* don't do magic till later */
	}
#ifdef HAS_SETRGID
	(void)setrgid((Gid_t)PL_gid);
#else
#ifdef HAS_SETREGID
	(void)setregid((Gid_t)PL_gid, (Gid_t)-1);
#else
#ifdef HAS_SETRESGID
      (void)setresgid((Gid_t)PL_gid, (Gid_t)-1, (Gid_t) 1);
#else
	if (PL_gid == PL_egid)			/* special case $( = $) */
	    (void)PerlProc_setgid(PL_gid);
	else {
	    PL_gid = (I32)PerlProc_getgid();
	    Perl_croak(aTHX_ "setrgid() not implemented");
	}
#endif
#endif
#endif
	PL_gid = (I32)PerlProc_getgid();
	PL_tainting |= (PL_uid && (PL_euid != PL_uid || PL_egid != PL_gid));
	break;
    case ')':
#ifdef HAS_SETGROUPS
	{
	    char *p = SvPV(sv, len);
	    Groups_t gary[NGROUPS];

	    while (isSPACE(*p))
		++p;
	    PL_egid = I_V(atol(p));
	    for (i = 0; i < NGROUPS; ++i) {
		while (*p && !isSPACE(*p))
		    ++p;
		while (isSPACE(*p))
		    ++p;
		if (!*p)
		    break;
		gary[i] = I_V(atol(p));
	    }
	    if (i)
		(void)setgroups(i, gary);
	}
#else  /* HAS_SETGROUPS */
	PL_egid = SvIOK(sv) ? SvIVX(sv) : sv_2iv(sv);
#endif /* HAS_SETGROUPS */
	if (PL_delaymagic) {
	    PL_delaymagic |= DM_EGID;
	    break;				/* don't do magic till later */
	}
#ifdef HAS_SETEGID
	(void)setegid((Gid_t)PL_egid);
#else
#ifdef HAS_SETREGID
	(void)setregid((Gid_t)-1, (Gid_t)PL_egid);
#else
#ifdef HAS_SETRESGID
	(void)setresgid((Gid_t)-1, (Gid_t)PL_egid, (Gid_t)-1);
#else
	if (PL_egid == PL_gid)			/* special case $) = $( */
	    (void)PerlProc_setgid(PL_egid);
	else {
	    PL_egid = (I32)PerlProc_getegid();
	    Perl_croak(aTHX_ "setegid() not implemented");
	}
#endif
#endif
#endif
	PL_egid = (I32)PerlProc_getegid();
	PL_tainting |= (PL_uid && (PL_euid != PL_uid || PL_egid != PL_gid));
	break;
    case ':':
	PL_chopset = SvPV_force(sv,len);
	break;
    case '0':
	if (!PL_origalen) {
	    s = PL_origargv[0];
	    s += strlen(s);
	    /* See if all the arguments are contiguous in memory */
	    for (i = 1; i < PL_origargc; i++) {
		if (PL_origargv[i] == s + 1
#ifdef OS2
		    || PL_origargv[i] == s + 2
#endif 
		   )
		{
		    ++s;
		    s += strlen(s);	/* this one is ok too */
		}
		else
		    break;
	    }
	    /* can grab env area too? */
	    if (PL_origenviron && (PL_origenviron[0] == s + 1
#ifdef OS2
				|| (PL_origenviron[0] == s + 9 && (s += 8))
#endif 
	       )) {
		my_setenv("NoNe  SuCh", Nullch);
					    /* force copy of environment */
		for (i = 0; PL_origenviron[i]; i++)
		    if (PL_origenviron[i] == s + 1) {
			++s;
			s += strlen(s);
		    }
		    else
			break;
	    }
	    PL_origalen = s - PL_origargv[0];
	}
	s = SvPV_force(sv,len);
	i = len;
	if (i >= PL_origalen) {
	    i = PL_origalen;
	    /* don't allow system to limit $0 seen by script */
	    /* SvCUR_set(sv, i); *SvEND(sv) = '\0'; */
	    Copy(s, PL_origargv[0], i, char);
	    s = PL_origargv[0]+i;
	    *s = '\0';
	}
	else {
	    Copy(s, PL_origargv[0], i, char);
	    s = PL_origargv[0]+i;
	    *s++ = '\0';
	    while (++i < PL_origalen)
		*s++ = ' ';
	    s = PL_origargv[0]+i;
	    for (i = 1; i < PL_origargc; i++)
		PL_origargv[i] = Nullch;
	}
	break;
#ifdef USE_THREADS
    case '@':
	sv_setsv(thr->errsv, sv);
	break;
#endif /* USE_THREADS */
    }
    return 0;
}

#ifdef USE_THREADS
int
Perl_magic_mutexfree(pTHX_ SV *sv, MAGIC *mg)
{
    dTHR;
    DEBUG_S(PerlIO_printf(PerlIO_stderr(), "0x%lx: magic_mutexfree 0x%lx\n",
			  (unsigned long)thr, (unsigned long)sv);)
    if (MgOWNER(mg))
	Perl_croak(aTHX_ "panic: magic_mutexfree");
    MUTEX_DESTROY(MgMUTEXP(mg));
    COND_DESTROY(MgCONDP(mg));
    return 0;
}
#endif /* USE_THREADS */

I32
Perl_whichsig(pTHX_ char *sig)
{
    register char **sigv;

    for (sigv = PL_sig_name+1; *sigv; sigv++)
	if (strEQ(sig,*sigv))
	    return PL_sig_num[sigv - PL_sig_name];
#ifdef SIGCLD
    if (strEQ(sig,"CHLD"))
	return SIGCLD;
#endif
#ifdef SIGCHLD
    if (strEQ(sig,"CLD"))
	return SIGCHLD;
#endif
    return 0;
}

static SV* sig_sv;

STATIC void
S_unwind_handler_stack(pTHX_ void *p)
{
    dTHR;
    U32 flags = *(U32*)p;

    if (flags & 1)
	PL_savestack_ix -= 5; /* Unprotect save in progress. */
    /* cxstack_ix-- Not needed, die already unwound it. */
    if (flags & 64)
	SvREFCNT_dec(sig_sv);
}

Signal_t
Perl_sighandler(int sig)
{
    dTHX;
    dSP;
    GV *gv = Nullgv;
    HV *st;
    SV *sv, *tSv = PL_Sv;
    CV *cv = Nullcv;
    OP *myop = PL_op;
    U32 flags = 0;
    I32 o_save_i = PL_savestack_ix, type;
    XPV *tXpv = PL_Xpv;
    
    if (PL_savestack_ix + 15 <= PL_savestack_max)
	flags |= 1;
    if (PL_markstack_ptr < PL_markstack_max - 2)
	flags |= 4;
    if (PL_retstack_ix < PL_retstack_max - 2)
	flags |= 8;
    if (PL_scopestack_ix < PL_scopestack_max - 3)
	flags |= 16;

    if (!PL_psig_ptr[sig])
	Perl_die(aTHX_ "Signal SIG%s received, but no signal handler set.\n",
	    PL_sig_name[sig]);

    /* Max number of items pushed there is 3*n or 4. We cannot fix
       infinity, so we fix 4 (in fact 5): */
    if (flags & 1) {
	PL_savestack_ix += 5;		/* Protect save in progress. */
	o_save_i = PL_savestack_ix;
	SAVEDESTRUCTOR(S_unwind_handler_stack, (void*)&flags);
    }
    if (flags & 4) 
	PL_markstack_ptr++;		/* Protect mark. */
    if (flags & 8) {
	PL_retstack_ix++;
	PL_retstack[PL_retstack_ix] = NULL;
    }
    if (flags & 16)
	PL_scopestack_ix += 1;
    /* sv_2cv is too complicated, try a simpler variant first: */
    if (!SvROK(PL_psig_ptr[sig]) || !(cv = (CV*)SvRV(PL_psig_ptr[sig])) 
	|| SvTYPE(cv) != SVt_PVCV)
	cv = sv_2cv(PL_psig_ptr[sig],&st,&gv,TRUE);

    if (!cv || !CvROOT(cv)) {
	if (ckWARN(WARN_SIGNAL))
	    Perl_warner(aTHX_ WARN_SIGNAL, "SIG%s handler \"%s\" not defined.\n",
		PL_sig_name[sig], (gv ? GvENAME(gv)
				: ((cv && CvGV(cv))
				   ? GvENAME(CvGV(cv))
				   : "__ANON__")));
	goto cleanup;
    }

    if(PL_psig_name[sig]) {
    	sv = SvREFCNT_inc(PL_psig_name[sig]);
	flags |= 64;
	sig_sv = sv;
    } else {
	sv = sv_newmortal();
	sv_setpv(sv,PL_sig_name[sig]);
    }

    PUSHSTACKi(PERLSI_SIGNAL);
    PUSHMARK(SP);
    PUSHs(sv);
    PUTBACK;

    call_sv((SV*)cv, G_DISCARD);

    POPSTACK;
cleanup:
    if (flags & 1)
	PL_savestack_ix -= 8; /* Unprotect save in progress. */
    if (flags & 4) 
	PL_markstack_ptr--;
    if (flags & 8) 
	PL_retstack_ix--;
    if (flags & 16)
	PL_scopestack_ix -= 1;
    if (flags & 64)
	SvREFCNT_dec(sv);
    PL_op = myop;			/* Apparently not needed... */
    
    PL_Sv = tSv;			/* Restore global temporaries. */
    PL_Xpv = tXpv;
    return;
}


