// Call handlers: flexible map of call targets to arbitrary handling code
//
// Each handler needs DEF_CALL_HANDLER and SETUP_CALL_HANDLER

typedef std::string (JSWriter::*CallHandler)(const CallInst*, std::string Name, int NumArgs);
typedef std::map<std::string, CallHandler> CallHandlerMap;
CallHandlerMap *CallHandlers;

// Definitions

#define DEF_CALL_HANDLER(Ident, Code) \
  std::string CH_##Ident(const CallInst *CI, std::string Name, int NumArgs=-1) { Code }

DEF_CALL_HANDLER(__default__, {
  const Value *CV = CI->getCalledValue();
  bool NeedCasts;
  if (const Function *F = dyn_cast<const Function>(CV)) {
    NeedCasts = F->isDeclaration(); // if ffi call, need casts
  } else {
    // function pointer call
    FunctionType *FT = dyn_cast<FunctionType>(dyn_cast<PointerType>(CV->getType())->getElementType());
    std::string Sig = getFunctionSignature(FT);
    Name = std::string("FUNCTION_TABLE_") + Sig + "[" + Name + " & #FM_" + Sig + "#]";
    ensureFunctionTable(FT);
    NeedCasts = false; // function table call, so stays in asm module
  }
  std::string text = Name + "(";
  if (NumArgs == -1) NumArgs = CI->getNumOperands()-1; // last operand is the function itself
  for (int i = 0; i < NumArgs; i++) {
    if (!NeedCasts) {
      text += getValueAsStr(CI->getArgOperand(i));
    } else {
      text += getValueAsCastStr(CI->getArgOperand(i), ASM_NONSPECIFIC);
    }
    if (i < NumArgs - 1) text += ", ";
  }
  text += ")";
  Type *RT = CI->getType();
  if (!RT->isVoidTy()) {
    text = getAssign(getJSName(CI), RT) + getCast(text, RT, ASM_NONSPECIFIC);
  }
  return text;
})

DEF_CALL_HANDLER(getHigh32, {
  return getAssign(getJSName(CI), CI->getType()) + "tempRet0";
})

DEF_CALL_HANDLER(setHigh32, {
  return "tempRet0 = " + getValueAsStr(CI->getArgOperand(0));
})

DEF_CALL_HANDLER(FPtoILow, {
  return getAssign(getJSName(CI), CI->getType()) + "(~~" + getValueAsStr(CI->getArgOperand(0)) + ")>>>0";
})

DEF_CALL_HANDLER(FPtoIHigh, {
  std::string Input = getValueAsStr(CI->getArgOperand(0));
  return getAssign(getJSName(CI), CI->getType()) + "+Math_abs(" + Input + ") >= +1 ? " + Input + " > +0 ? (Math_min(+Math_floor(" + Input + " / +4294967296), +4294967295) | 0) >>> 0 : ~~+Math_ceil((" + Input + " - +(~~" + Input + " >>> 0)) / +4294967296) >>> 0 : 0";
})

DEF_CALL_HANDLER(BDtoILow, {
  return "HEAPF64[tempDoublePtr>>3] = " + getValueAsStr(CI->getArgOperand(0)) + ";" + getAssign(getJSName(CI), CI->getType()) + "HEAP32[tempDoublePtr>>2]|0";
})

DEF_CALL_HANDLER(BDtoIHigh, {
  return getAssign(getJSName(CI), CI->getType()) + "HEAP32[tempDoublePtr+4>>2]|0";
})

DEF_CALL_HANDLER(SItoF, {
  // TODO: fround
  return getAssign(getJSName(CI), CI->getType()) + "(+" + getValueAsCastParenStr(CI->getArgOperand(0), ASM_UNSIGNED) + ") + " +
                                       "(+4294967296*(+" + getValueAsCastParenStr(CI->getArgOperand(1), ASM_SIGNED) +   "))";
})

DEF_CALL_HANDLER(UItoF, {
  // TODO: fround
  return getAssign(getJSName(CI), CI->getType()) + "(+" + getValueAsCastParenStr(CI->getArgOperand(0), ASM_UNSIGNED) + ") + " +
                                       "(+4294967296*(+" + getValueAsCastParenStr(CI->getArgOperand(1), ASM_UNSIGNED) + "))";
})

DEF_CALL_HANDLER(SItoD, {
  return getAssign(getJSName(CI), CI->getType()) + "(+" + getValueAsCastParenStr(CI->getArgOperand(0), ASM_UNSIGNED) + ") + " +
                                       "(+4294967296*(+" + getValueAsCastParenStr(CI->getArgOperand(1), ASM_SIGNED) +   "))";
})

DEF_CALL_HANDLER(UItoD, {
  return getAssign(getJSName(CI), CI->getType()) + "(+" + getValueAsCastParenStr(CI->getArgOperand(0), ASM_UNSIGNED) + ") + " +
                                       "(+4294967296*(+" + getValueAsCastParenStr(CI->getArgOperand(1), ASM_UNSIGNED) + "))";
})

DEF_CALL_HANDLER(BItoD, {
  return "HEAP32[tempDoublePtr>>2] = " +   getValueAsStr(CI->getArgOperand(0)) + ";" +
         "HEAP32[tempDoublePtr+4>>2] = " + getValueAsStr(CI->getArgOperand(1)) + ";" +
         getAssign(getJSName(CI), CI->getType()) + "+HEAPF64[tempDoublePtr>>3]";
})

DEF_CALL_HANDLER(llvm_nacl_atomic_store_i32, {
  return "HEAP32[" + getValueAsStr(CI->getArgOperand(0)) + ">>2]=" + getValueAsStr(CI->getArgOperand(1));
})

DEF_CALL_HANDLER(llvm_memcpy_p0i8_p0i8_i32, {
  Declares.insert("memcpy");
  return CH___default__(CI, "_memcpy", 3) + "|0";
})

DEF_CALL_HANDLER(llvm_memset_p0i8_i32, {
  Declares.insert("memset");
  return CH___default__(CI, "_memset", 3) + "|0";
})

DEF_CALL_HANDLER(llvm_memmove_p0i8_p0i8_i32, {
  Declares.insert("memmove");
  return CH___default__(CI, "_memmove", 3) + "|0";
})

DEF_CALL_HANDLER(llvm_expect_i32, {
  return getAssign(getJSName(CI), CI->getType()) + getValueAsStr(CI->getArgOperand(0));
})

DEF_CALL_HANDLER(llvm_dbg_declare, {
  return "";
})

DEF_CALL_HANDLER(bitshift64Lshr, {
  return CH___default__(CI, "_bitshift64Lshr", 3);
})

DEF_CALL_HANDLER(bitshift64Ashr, {
  return CH___default__(CI, "_bitshift64Ashr", 3);
})

DEF_CALL_HANDLER(bitshift64Shl, {
  return CH___default__(CI, "_bitshift64Shl", 3);
})

DEF_CALL_HANDLER(llvm_ctlz_i32, {
  return CH___default__(CI, "_llvm_ctlz_i32", 1);
})

DEF_CALL_HANDLER(llvm_cttz_i32, {
  return CH___default__(CI, "_llvm_cttz_i32", 1);
})

#define DEF_REDIRECT_HANDLER(name, to) \
DEF_CALL_HANDLER(name, { \
  /* FIXME: do not redirect if this is implemented and not just a declare! */ \
  Declares.insert(#to); \
  Redirects[#name] = #to; \
  if (!CI) return ""; \
  return CH___default__(CI, "_" #to); \
})

#define DEF_BUILTIN_HANDLER(name, to) \
DEF_CALL_HANDLER(name, { \
  if (!CI) return ""; \
  return CH___default__(CI, #to); \
})

// Various simple redirects for our js libc, see library.js and LibraryManager.load
DEF_REDIRECT_HANDLER(__01readdir64_, readdir);
DEF_REDIRECT_HANDLER(__xpg_basename, basename);
DEF_REDIRECT_HANDLER(stat64, stat);
DEF_REDIRECT_HANDLER(fstat64, fstat);
DEF_REDIRECT_HANDLER(lstat64, lstat);
DEF_REDIRECT_HANDLER(__01fstat64_, fstat);
DEF_REDIRECT_HANDLER(__01stat64_, stat);
DEF_REDIRECT_HANDLER(__01lstat64_, lstat);
DEF_REDIRECT_HANDLER(__01statvfs64_, statvfs);
DEF_REDIRECT_HANDLER(__01fstatvfs64_, fstatvfs);
DEF_REDIRECT_HANDLER(pathconf, fpathconf);
DEF_REDIRECT_HANDLER(fdatasync, fsync);
DEF_REDIRECT_HANDLER(ualarm, alarm);
DEF_REDIRECT_HANDLER(execle, execl);
DEF_REDIRECT_HANDLER(execlp, execl);
DEF_REDIRECT_HANDLER(execv, execl);
DEF_REDIRECT_HANDLER(execve, execl);
DEF_REDIRECT_HANDLER(execvp, execl);
DEF_REDIRECT_HANDLER(vfork, fork);
DEF_REDIRECT_HANDLER(getegid, getgid);
DEF_REDIRECT_HANDLER(getuid, getgid);
DEF_REDIRECT_HANDLER(geteuid, getgid);
DEF_REDIRECT_HANDLER(getpgrp, getgid);
DEF_REDIRECT_HANDLER(getpid, getgid);
DEF_REDIRECT_HANDLER(getppid, getgid);
DEF_REDIRECT_HANDLER(getresgid, getresuid);
DEF_REDIRECT_HANDLER(getsid, getpgid);
DEF_REDIRECT_HANDLER(setegid, setgid);
DEF_REDIRECT_HANDLER(setuid, setgid);
DEF_REDIRECT_HANDLER(seteuid, setgid);
DEF_REDIRECT_HANDLER(setsid, setgid);
DEF_REDIRECT_HANDLER(setpgrp, setgid);
DEF_REDIRECT_HANDLER(setregid, setpgid);
DEF_REDIRECT_HANDLER(setreuid, setpgid);
DEF_REDIRECT_HANDLER(setresuid, setpgid);
DEF_REDIRECT_HANDLER(setresgid, setpgid);
DEF_REDIRECT_HANDLER(open64, open);
DEF_REDIRECT_HANDLER(lseek64, lseek);
DEF_REDIRECT_HANDLER(ftruncate64, ftruncate);
DEF_REDIRECT_HANDLER(__01open64_, open);
DEF_REDIRECT_HANDLER(__01lseek64_, lseek);
DEF_REDIRECT_HANDLER(__01truncate64_, truncate);
DEF_REDIRECT_HANDLER(__01ftruncate64_, ftruncate);
DEF_REDIRECT_HANDLER(getc, fgetc);
DEF_REDIRECT_HANDLER(getc_unlocked, fgetc);
DEF_REDIRECT_HANDLER(flockfile, ftrylockfile);
DEF_REDIRECT_HANDLER(funlockfile, ftrylockfile);
DEF_REDIRECT_HANDLER(putc, fputc);
DEF_REDIRECT_HANDLER(putc_unlocked, fputc);
DEF_REDIRECT_HANDLER(putchar_unlocked, putchar);
DEF_REDIRECT_HANDLER(fseeko, fseek);
DEF_REDIRECT_HANDLER(fseeko64, fseek);
DEF_REDIRECT_HANDLER(ftello, ftell);
DEF_REDIRECT_HANDLER(ftello64, ftell);
DEF_REDIRECT_HANDLER(fopen64, fopen);
DEF_REDIRECT_HANDLER(__01fopen64_, fopen);
DEF_REDIRECT_HANDLER(__01freopen64_, freopen);
DEF_REDIRECT_HANDLER(__01fseeko64_, fseek);
DEF_REDIRECT_HANDLER(__01ftello64_, ftell);
DEF_REDIRECT_HANDLER(__01tmpfile64_, tmpfile);
DEF_REDIRECT_HANDLER(__isoc99_fscanf, fscanf);
DEF_REDIRECT_HANDLER(_IO_getc, getc);
DEF_REDIRECT_HANDLER(_IO_putc, putc);
DEF_REDIRECT_HANDLER(_ZNSo3putEc, putchar);
DEF_REDIRECT_HANDLER(__01mmap64_, mmap);
DEF_BUILTIN_HANDLER(abs, Math_abs);
DEF_BUILTIN_HANDLER(labs, Math_abs);
DEF_REDIRECT_HANDLER(__cxa_atexit, atexit);
DEF_REDIRECT_HANDLER(strtoll_l, strtoll);
DEF_REDIRECT_HANDLER(strtol_l, strtol);
DEF_REDIRECT_HANDLER(strtoul_l, strtoul);
DEF_REDIRECT_HANDLER(strtoull_l, strtoull);
DEF_REDIRECT_HANDLER(atol, atoi);
DEF_REDIRECT_HANDLER(__environ, _environ);
DEF_REDIRECT_HANDLER(arc4random, rand);
DEF_REDIRECT_HANDLER(llvm_memcpy_i32, memcpy);
DEF_REDIRECT_HANDLER(llvm_memcpy_i64, memcpy);
DEF_REDIRECT_HANDLER(llvm_memcpy_p0i8_p0i8_i64, memcpy);
DEF_REDIRECT_HANDLER(llvm_memmove_i32, memmove);
DEF_REDIRECT_HANDLER(llvm_memmove_i64, memmove);
DEF_REDIRECT_HANDLER(llvm_memmove_p0i8_p0i8_i64, memmove);
DEF_REDIRECT_HANDLER(llvm_memset_i32, memset);
DEF_REDIRECT_HANDLER(llvm_memset_p0i8_i64, memset);
DEF_REDIRECT_HANDLER(strcoll, strcmp);
DEF_REDIRECT_HANDLER(strcoll_l, strcmp);
DEF_REDIRECT_HANDLER(index, strchr);
DEF_REDIRECT_HANDLER(rindex, strrchr);
DEF_REDIRECT_HANDLER(_toupper, toupper);
DEF_REDIRECT_HANDLER(toupper_l, toupper);
DEF_REDIRECT_HANDLER(_tolower, tolower);
DEF_REDIRECT_HANDLER(tolower_l, tolower);
DEF_REDIRECT_HANDLER(islower_l, islower);
DEF_REDIRECT_HANDLER(isupper_l, isupper);
DEF_REDIRECT_HANDLER(isalpha_l, isalpha);
DEF_REDIRECT_HANDLER(isdigit_l, isdigit);
DEF_REDIRECT_HANDLER(isxdigit_l, isxdigit);
DEF_REDIRECT_HANDLER(isalnum_l, isalnum);
DEF_REDIRECT_HANDLER(ispunct_l, ispunct);
DEF_REDIRECT_HANDLER(isspace_l, isspace);
DEF_REDIRECT_HANDLER(isblank_l, isblank);
DEF_REDIRECT_HANDLER(iscntrl_l, iscntrl);
DEF_REDIRECT_HANDLER(isprint_l, isprint);
DEF_REDIRECT_HANDLER(isgraph_l, isgraph);
DEF_REDIRECT_HANDLER(terminate, __cxa_call_unexpected);
DEF_BUILTIN_HANDLER(cos, Math_cos);
DEF_BUILTIN_HANDLER(cosf, Math_cos);
DEF_BUILTIN_HANDLER(cosl, Math_cos);
DEF_BUILTIN_HANDLER(sin, Math_sin);
DEF_BUILTIN_HANDLER(sinf, Math_sin);
DEF_BUILTIN_HANDLER(sinl, Math_sin);
DEF_BUILTIN_HANDLER(tan, Math_tan);
DEF_BUILTIN_HANDLER(tanf, Math_tan);
DEF_BUILTIN_HANDLER(tanl, Math_tan);
DEF_BUILTIN_HANDLER(acos, Math_acos);
DEF_BUILTIN_HANDLER(acosf, Math_acos);
DEF_BUILTIN_HANDLER(acosl, Math_acos);
DEF_BUILTIN_HANDLER(asin, Math_asin);
DEF_BUILTIN_HANDLER(asinf, Math_asin);
DEF_BUILTIN_HANDLER(asinl, Math_asin);
DEF_BUILTIN_HANDLER(atan, Math_atan);
DEF_BUILTIN_HANDLER(atanf, Math_atan);
DEF_BUILTIN_HANDLER(atanl, Math_atan);
DEF_BUILTIN_HANDLER(atan2, Math_atan2);
DEF_BUILTIN_HANDLER(atan2f, Math_atan2);
DEF_BUILTIN_HANDLER(atan2l, Math_atan2);
DEF_BUILTIN_HANDLER(exp, Math_exp);
DEF_BUILTIN_HANDLER(expf, Math_exp);
DEF_BUILTIN_HANDLER(expl, Math_exp);
DEF_REDIRECT_HANDLER(erfcf, erfc);
DEF_REDIRECT_HANDLER(erfcl, erfc);
DEF_REDIRECT_HANDLER(erff, erf);
DEF_REDIRECT_HANDLER(erfl, erf);
DEF_BUILTIN_HANDLER(log, Math_log);
DEF_BUILTIN_HANDLER(logf, Math_log);
DEF_BUILTIN_HANDLER(logl, Math_log);
DEF_BUILTIN_HANDLER(sqrt, Math_sqrt);
DEF_BUILTIN_HANDLER(sqrtf, Math_sqrt);
DEF_BUILTIN_HANDLER(sqrtl, Math_sqrt);
DEF_BUILTIN_HANDLER(fabs, Math_abs);
DEF_BUILTIN_HANDLER(fabsf, Math_abs);
DEF_BUILTIN_HANDLER(fabsl, Math_abs);
DEF_BUILTIN_HANDLER(ceil, Math_ceil);
DEF_BUILTIN_HANDLER(ceilf, Math_ceil);
DEF_BUILTIN_HANDLER(ceill, Math_ceil);
DEF_BUILTIN_HANDLER(floor, Math_floor);
DEF_BUILTIN_HANDLER(floorf, Math_floor);
DEF_BUILTIN_HANDLER(floorl, Math_floor);
DEF_BUILTIN_HANDLER(pow, Math_pow);
DEF_BUILTIN_HANDLER(powf, Math_pow);
DEF_BUILTIN_HANDLER(powl, Math_pow);
DEF_BUILTIN_HANDLER(llvm_sqrt_f32, Math_sqrt);
DEF_BUILTIN_HANDLER(llvm_sqrt_f64, Math_sqrt);
DEF_BUILTIN_HANDLER(llvm_pow_f32, Math_pow);
DEF_BUILTIN_HANDLER(llvm_pow_f64, Math_pow);
DEF_BUILTIN_HANDLER(llvm_log_f32, Math_log);
DEF_BUILTIN_HANDLER(llvm_log_f64, Math_log);
DEF_BUILTIN_HANDLER(llvm_exp_f32, Math_exp);
DEF_BUILTIN_HANDLER(llvm_exp_f64, Math_exp);
DEF_REDIRECT_HANDLER(ldexpf, ldexp);
DEF_REDIRECT_HANDLER(scalb, ldexp);
DEF_REDIRECT_HANDLER(scalbn, ldexp);
DEF_REDIRECT_HANDLER(scalbnf, ldexp);
DEF_REDIRECT_HANDLER(scalbln, ldexp);
DEF_REDIRECT_HANDLER(scalblnf, ldexp);
DEF_REDIRECT_HANDLER(cbrtf, cbrt);
DEF_REDIRECT_HANDLER(cbrtl, cbrt);
DEF_REDIRECT_HANDLER(frexpf, frexp);
DEF_REDIRECT_HANDLER(__finite, finite);
DEF_REDIRECT_HANDLER(__isinf, isinf);
DEF_REDIRECT_HANDLER(__isnan, isnan);
DEF_REDIRECT_HANDLER(copysignf, copysign);
DEF_REDIRECT_HANDLER(copysignl, copysign);
DEF_REDIRECT_HANDLER(hypotf, hypot);
DEF_REDIRECT_HANDLER(hypotl, hypot);
DEF_REDIRECT_HANDLER(sinhf, sinh);
DEF_REDIRECT_HANDLER(sinhl, sinh);
DEF_REDIRECT_HANDLER(coshf, cosh);
DEF_REDIRECT_HANDLER(coshl, cosh);
DEF_REDIRECT_HANDLER(tanhf, tanh);
DEF_REDIRECT_HANDLER(tanhl, tanh);
DEF_REDIRECT_HANDLER(asinhf, asinh);
DEF_REDIRECT_HANDLER(asinhl, asinh);
DEF_REDIRECT_HANDLER(acoshf, acosh);
DEF_REDIRECT_HANDLER(acoshl, acosh);
DEF_REDIRECT_HANDLER(atanhf, atanh);
DEF_REDIRECT_HANDLER(atanhl, atanh);
DEF_REDIRECT_HANDLER(exp2f, exp2);
DEF_REDIRECT_HANDLER(exp2l, exp2);
DEF_REDIRECT_HANDLER(expm1f, expm1);
DEF_REDIRECT_HANDLER(expm1l, expm1);
DEF_REDIRECT_HANDLER(roundf, round);
DEF_REDIRECT_HANDLER(roundl, round);
DEF_REDIRECT_HANDLER(lround, round);
DEF_REDIRECT_HANDLER(lroundf, round);
DEF_REDIRECT_HANDLER(lroundl, round);
DEF_REDIRECT_HANDLER(llround, round);
DEF_REDIRECT_HANDLER(llroundf, round);
DEF_REDIRECT_HANDLER(llroundl, round);
DEF_REDIRECT_HANDLER(rintf, rint);
DEF_REDIRECT_HANDLER(rintl, rint);
DEF_REDIRECT_HANDLER(lrint, rint);
DEF_REDIRECT_HANDLER(lrintf, rint);
DEF_REDIRECT_HANDLER(lrintl, rint);
DEF_REDIRECT_HANDLER(llrintf, llrint);
DEF_REDIRECT_HANDLER(llrintl, llrint);
DEF_REDIRECT_HANDLER(nearbyint, rint);
DEF_REDIRECT_HANDLER(nearbyintf, rint);
DEF_REDIRECT_HANDLER(nearbyintl, rint);
DEF_REDIRECT_HANDLER(truncf, trunc);
DEF_REDIRECT_HANDLER(truncl, trunc);
DEF_REDIRECT_HANDLER(fdimf, fdim);
DEF_REDIRECT_HANDLER(fdiml, fdim);
DEF_REDIRECT_HANDLER(fmaxf, fmax);
DEF_REDIRECT_HANDLER(fmaxl, fmax);
DEF_REDIRECT_HANDLER(fminf, fmin);
DEF_REDIRECT_HANDLER(fminl, fmin);
DEF_REDIRECT_HANDLER(fmaf, fma);
DEF_REDIRECT_HANDLER(fmal, fma);
DEF_REDIRECT_HANDLER(fmodf, fmod);
DEF_REDIRECT_HANDLER(fmodl, fmod);
DEF_REDIRECT_HANDLER(remainder, fmod);
DEF_REDIRECT_HANDLER(remainderf, fmod);
DEF_REDIRECT_HANDLER(remainderl, fmod);
DEF_REDIRECT_HANDLER(log10f, log10);
DEF_REDIRECT_HANDLER(log10l, log10);
DEF_REDIRECT_HANDLER(log1pf, log1p);
DEF_REDIRECT_HANDLER(log1pl, log1p);
DEF_REDIRECT_HANDLER(log2f, log2);
DEF_REDIRECT_HANDLER(log2l, log2);
DEF_REDIRECT_HANDLER(nanf, nan);
DEF_REDIRECT_HANDLER(nanl, nan);
DEF_REDIRECT_HANDLER(sincosl, sincos);
DEF_REDIRECT_HANDLER(__fpclassifyd, __fpclassify);
DEF_REDIRECT_HANDLER(__fpclassifyf, __fpclassify);
DEF_REDIRECT_HANDLER(__fpclassifyl, __fpclassify);
DEF_REDIRECT_HANDLER(timelocal, mktime);
DEF_REDIRECT_HANDLER(strftime_l, strftime);
DEF_REDIRECT_HANDLER(strptime_l, strptime);
DEF_REDIRECT_HANDLER(gnu_dev_makedev, makedev);
DEF_REDIRECT_HANDLER(gnu_dev_major, major);
DEF_REDIRECT_HANDLER(gnu_dev_minor, minor);
DEF_REDIRECT_HANDLER(sigprocmask, sigaction);
DEF_REDIRECT_HANDLER(killpg, kill);
DEF_REDIRECT_HANDLER(waitid, wait);
DEF_REDIRECT_HANDLER(waitpid, wait);
DEF_REDIRECT_HANDLER(wait3, wait);
DEF_REDIRECT_HANDLER(wait4, wait);
DEF_REDIRECT_HANDLER(__errno, __errno_location);
DEF_REDIRECT_HANDLER(__01getrlimit64_, getrlimit);
DEF_REDIRECT_HANDLER(ntohl, htonl);
DEF_REDIRECT_HANDLER(ntohs, htons);
DEF_REDIRECT_HANDLER(SDL_LoadBMP, IMG_Load);
DEF_REDIRECT_HANDLER(SDL_LoadBMP_RW, IMG_Load_RW);
DEF_REDIRECT_HANDLER(Mix_CloseAudio, SDL_CloseAudio);
DEF_REDIRECT_HANDLER(Mix_PlayChannelTimed, Mix_PlayChannel);
DEF_REDIRECT_HANDLER(Mix_LoadMUS_RW, Mix_LoadWAV_RW);
DEF_REDIRECT_HANDLER(Mix_FreeMusic, Mix_FreeChunk);
DEF_REDIRECT_HANDLER(Mix_FadeInMusicPos, Mix_PlayMusic);
DEF_REDIRECT_HANDLER(Mix_FadeOutMusic, Mix_HaltMusic);
DEF_REDIRECT_HANDLER(TTF_RenderText_Blended, TTF_RenderText_Solid);
DEF_REDIRECT_HANDLER(TTF_RenderText_Shaded, TTF_RenderText_Solid);
DEF_REDIRECT_HANDLER(TTF_RenderUTF8_Solid, TTF_RenderText_Solid);
DEF_REDIRECT_HANDLER(SDL_getenv, getenv);
DEF_REDIRECT_HANDLER(SDL_RWFromMem, SDL_RWFromConstMem);

// Setups

void setupCallHandlers() {
  CallHandlers = new CallHandlerMap;
  #define SETUP_CALL_HANDLER(Ident) \
    (*CallHandlers)[std::string("_") + #Ident] = &JSWriter::CH_##Ident;

  SETUP_CALL_HANDLER(__default__);
  SETUP_CALL_HANDLER(getHigh32);
  SETUP_CALL_HANDLER(setHigh32);
  SETUP_CALL_HANDLER(FPtoILow);
  SETUP_CALL_HANDLER(FPtoIHigh);
  SETUP_CALL_HANDLER(BDtoILow);
  SETUP_CALL_HANDLER(BDtoIHigh);
  SETUP_CALL_HANDLER(SItoF);
  SETUP_CALL_HANDLER(UItoF);
  SETUP_CALL_HANDLER(SItoD);
  SETUP_CALL_HANDLER(UItoD);
  SETUP_CALL_HANDLER(BItoD);
  SETUP_CALL_HANDLER(llvm_nacl_atomic_store_i32);
  SETUP_CALL_HANDLER(llvm_memcpy_p0i8_p0i8_i32);
  SETUP_CALL_HANDLER(llvm_memset_p0i8_i32);
  SETUP_CALL_HANDLER(llvm_memmove_p0i8_p0i8_i32);
  SETUP_CALL_HANDLER(llvm_expect_i32);
  SETUP_CALL_HANDLER(llvm_dbg_declare);
  SETUP_CALL_HANDLER(bitshift64Lshr);
  SETUP_CALL_HANDLER(bitshift64Ashr);
  SETUP_CALL_HANDLER(bitshift64Shl);
  SETUP_CALL_HANDLER(llvm_ctlz_i32);
  SETUP_CALL_HANDLER(llvm_cttz_i32);

  SETUP_CALL_HANDLER(__01readdir64_);
  SETUP_CALL_HANDLER(__xpg_basename);
  SETUP_CALL_HANDLER(stat64);
  SETUP_CALL_HANDLER(fstat64);
  SETUP_CALL_HANDLER(lstat64);
  SETUP_CALL_HANDLER(__01fstat64_);
  SETUP_CALL_HANDLER(__01stat64_);
  SETUP_CALL_HANDLER(__01lstat64_);
  SETUP_CALL_HANDLER(__01statvfs64_);
  SETUP_CALL_HANDLER(__01fstatvfs64_);
  SETUP_CALL_HANDLER(pathconf);
  SETUP_CALL_HANDLER(fdatasync);
  SETUP_CALL_HANDLER(ualarm);
  SETUP_CALL_HANDLER(execle);
  SETUP_CALL_HANDLER(execlp);
  SETUP_CALL_HANDLER(execv);
  SETUP_CALL_HANDLER(execve);
  SETUP_CALL_HANDLER(execvp);
  SETUP_CALL_HANDLER(vfork);
  SETUP_CALL_HANDLER(getegid);
  SETUP_CALL_HANDLER(getuid);
  SETUP_CALL_HANDLER(geteuid);
  SETUP_CALL_HANDLER(getpgrp);
  SETUP_CALL_HANDLER(getpid);
  SETUP_CALL_HANDLER(getppid);
  SETUP_CALL_HANDLER(getresgid);
  SETUP_CALL_HANDLER(getsid);
  SETUP_CALL_HANDLER(setegid);
  SETUP_CALL_HANDLER(setuid);
  SETUP_CALL_HANDLER(seteuid);
  SETUP_CALL_HANDLER(setsid);
  SETUP_CALL_HANDLER(setpgrp);
  SETUP_CALL_HANDLER(setregid);
  SETUP_CALL_HANDLER(setreuid);
  SETUP_CALL_HANDLER(setresuid);
  SETUP_CALL_HANDLER(setresgid);
  SETUP_CALL_HANDLER(open64);
  SETUP_CALL_HANDLER(lseek64);
  SETUP_CALL_HANDLER(ftruncate64);
  SETUP_CALL_HANDLER(__01open64_);
  SETUP_CALL_HANDLER(__01lseek64_);
  SETUP_CALL_HANDLER(__01truncate64_);
  SETUP_CALL_HANDLER(__01ftruncate64_);
  SETUP_CALL_HANDLER(getc);
  SETUP_CALL_HANDLER(getc_unlocked);
  SETUP_CALL_HANDLER(flockfile);
  SETUP_CALL_HANDLER(funlockfile);
  SETUP_CALL_HANDLER(putc);
  SETUP_CALL_HANDLER(putc_unlocked);
  SETUP_CALL_HANDLER(putchar_unlocked);
  SETUP_CALL_HANDLER(fseeko);
  SETUP_CALL_HANDLER(fseeko64);
  SETUP_CALL_HANDLER(ftello);
  SETUP_CALL_HANDLER(ftello64);
  SETUP_CALL_HANDLER(fopen64);
  SETUP_CALL_HANDLER(__01fopen64_);
  SETUP_CALL_HANDLER(__01freopen64_);
  SETUP_CALL_HANDLER(__01fseeko64_);
  SETUP_CALL_HANDLER(__01ftello64_);
  SETUP_CALL_HANDLER(__01tmpfile64_);
  SETUP_CALL_HANDLER(__isoc99_fscanf);
  SETUP_CALL_HANDLER(_IO_getc);
  SETUP_CALL_HANDLER(_IO_putc);
  SETUP_CALL_HANDLER(_ZNSo3putEc);
  SETUP_CALL_HANDLER(__01mmap64_);
  SETUP_CALL_HANDLER(abs);
  SETUP_CALL_HANDLER(labs);
  SETUP_CALL_HANDLER(__cxa_atexit);
  SETUP_CALL_HANDLER(strtoll_l);
  SETUP_CALL_HANDLER(strtol_l);
  SETUP_CALL_HANDLER(strtoul_l);
  SETUP_CALL_HANDLER(strtoull_l);
  SETUP_CALL_HANDLER(atol);
  SETUP_CALL_HANDLER(__environ);
  SETUP_CALL_HANDLER(arc4random);
  SETUP_CALL_HANDLER(llvm_memcpy_i32);
  SETUP_CALL_HANDLER(llvm_memcpy_i64);
  SETUP_CALL_HANDLER(llvm_memcpy_p0i8_p0i8_i64);
  SETUP_CALL_HANDLER(llvm_memmove_i32);
  SETUP_CALL_HANDLER(llvm_memmove_i64);
  SETUP_CALL_HANDLER(llvm_memmove_p0i8_p0i8_i64);
  SETUP_CALL_HANDLER(llvm_memset_i32);
  SETUP_CALL_HANDLER(llvm_memset_p0i8_i64);
  SETUP_CALL_HANDLER(strcoll);
  SETUP_CALL_HANDLER(strcoll_l);
  SETUP_CALL_HANDLER(index);
  SETUP_CALL_HANDLER(rindex);
  SETUP_CALL_HANDLER(_toupper);
  SETUP_CALL_HANDLER(toupper_l);
  SETUP_CALL_HANDLER(_tolower);
  SETUP_CALL_HANDLER(tolower_l);
  SETUP_CALL_HANDLER(islower_l);
  SETUP_CALL_HANDLER(isupper_l);
  SETUP_CALL_HANDLER(isalpha_l);
  SETUP_CALL_HANDLER(isdigit_l);
  SETUP_CALL_HANDLER(isxdigit_l);
  SETUP_CALL_HANDLER(isalnum_l);
  SETUP_CALL_HANDLER(ispunct_l);
  SETUP_CALL_HANDLER(isspace_l);
  SETUP_CALL_HANDLER(isblank_l);
  SETUP_CALL_HANDLER(iscntrl_l);
  SETUP_CALL_HANDLER(isprint_l);
  SETUP_CALL_HANDLER(isgraph_l);
  SETUP_CALL_HANDLER(terminate);
  SETUP_CALL_HANDLER(cos);
  SETUP_CALL_HANDLER(cosf);
  SETUP_CALL_HANDLER(cosl);
  SETUP_CALL_HANDLER(sin);
  SETUP_CALL_HANDLER(sinf);
  SETUP_CALL_HANDLER(sinl);
  SETUP_CALL_HANDLER(tan);
  SETUP_CALL_HANDLER(tanf);
  SETUP_CALL_HANDLER(tanl);
  SETUP_CALL_HANDLER(acos);
  SETUP_CALL_HANDLER(acosf);
  SETUP_CALL_HANDLER(acosl);
  SETUP_CALL_HANDLER(asin);
  SETUP_CALL_HANDLER(asinf);
  SETUP_CALL_HANDLER(asinl);
  SETUP_CALL_HANDLER(atan);
  SETUP_CALL_HANDLER(atanf);
  SETUP_CALL_HANDLER(atanl);
  SETUP_CALL_HANDLER(atan2);
  SETUP_CALL_HANDLER(atan2f);
  SETUP_CALL_HANDLER(atan2l);
  SETUP_CALL_HANDLER(exp);
  SETUP_CALL_HANDLER(expf);
  SETUP_CALL_HANDLER(expl);
  SETUP_CALL_HANDLER(erfcf);
  SETUP_CALL_HANDLER(erfcl);
  SETUP_CALL_HANDLER(erff);
  SETUP_CALL_HANDLER(erfl);
  SETUP_CALL_HANDLER(log);
  SETUP_CALL_HANDLER(logf);
  SETUP_CALL_HANDLER(logl);
  SETUP_CALL_HANDLER(sqrt);
  SETUP_CALL_HANDLER(sqrtf);
  SETUP_CALL_HANDLER(sqrtl);
  SETUP_CALL_HANDLER(fabs);
  SETUP_CALL_HANDLER(fabsf);
  SETUP_CALL_HANDLER(fabsl);
  SETUP_CALL_HANDLER(ceil);
  SETUP_CALL_HANDLER(ceilf);
  SETUP_CALL_HANDLER(ceill);
  SETUP_CALL_HANDLER(floor);
  SETUP_CALL_HANDLER(floorf);
  SETUP_CALL_HANDLER(floorl);
  SETUP_CALL_HANDLER(pow);
  SETUP_CALL_HANDLER(powf);
  SETUP_CALL_HANDLER(powl);
  SETUP_CALL_HANDLER(llvm_sqrt_f32);
  SETUP_CALL_HANDLER(llvm_sqrt_f64);
  SETUP_CALL_HANDLER(llvm_pow_f32);
  SETUP_CALL_HANDLER(llvm_pow_f64);
  SETUP_CALL_HANDLER(llvm_log_f32);
  SETUP_CALL_HANDLER(llvm_log_f64);
  SETUP_CALL_HANDLER(llvm_exp_f32);
  SETUP_CALL_HANDLER(llvm_exp_f64);
  SETUP_CALL_HANDLER(ldexpf);
  SETUP_CALL_HANDLER(scalb);
  SETUP_CALL_HANDLER(scalbn);
  SETUP_CALL_HANDLER(scalbnf);
  SETUP_CALL_HANDLER(scalbln);
  SETUP_CALL_HANDLER(scalblnf);
  SETUP_CALL_HANDLER(cbrtf);
  SETUP_CALL_HANDLER(cbrtl);
  SETUP_CALL_HANDLER(frexpf);
  SETUP_CALL_HANDLER(__finite);
  SETUP_CALL_HANDLER(__isinf);
  SETUP_CALL_HANDLER(__isnan);
  SETUP_CALL_HANDLER(copysignf);
  SETUP_CALL_HANDLER(copysignl);
  SETUP_CALL_HANDLER(hypotf);
  SETUP_CALL_HANDLER(hypotl);
  SETUP_CALL_HANDLER(sinhf);
  SETUP_CALL_HANDLER(sinhl);
  SETUP_CALL_HANDLER(coshf);
  SETUP_CALL_HANDLER(coshl);
  SETUP_CALL_HANDLER(tanhf);
  SETUP_CALL_HANDLER(tanhl);
  SETUP_CALL_HANDLER(asinhf);
  SETUP_CALL_HANDLER(asinhl);
  SETUP_CALL_HANDLER(acoshf);
  SETUP_CALL_HANDLER(acoshl);
  SETUP_CALL_HANDLER(atanhf);
  SETUP_CALL_HANDLER(atanhl);
  SETUP_CALL_HANDLER(exp2f);
  SETUP_CALL_HANDLER(exp2l);
  SETUP_CALL_HANDLER(expm1f);
  SETUP_CALL_HANDLER(expm1l);
  SETUP_CALL_HANDLER(roundf);
  SETUP_CALL_HANDLER(roundl);
  SETUP_CALL_HANDLER(lround);
  SETUP_CALL_HANDLER(lroundf);
  SETUP_CALL_HANDLER(lroundl);
  SETUP_CALL_HANDLER(llround);
  SETUP_CALL_HANDLER(llroundf);
  SETUP_CALL_HANDLER(llroundl);
  SETUP_CALL_HANDLER(rintf);
  SETUP_CALL_HANDLER(rintl);
  SETUP_CALL_HANDLER(lrint);
  SETUP_CALL_HANDLER(lrintf);
  SETUP_CALL_HANDLER(lrintl);
  SETUP_CALL_HANDLER(llrintf);
  SETUP_CALL_HANDLER(llrintl);
  SETUP_CALL_HANDLER(nearbyint);
  SETUP_CALL_HANDLER(nearbyintf);
  SETUP_CALL_HANDLER(nearbyintl);
  SETUP_CALL_HANDLER(truncf);
  SETUP_CALL_HANDLER(truncl);
  SETUP_CALL_HANDLER(fdimf);
  SETUP_CALL_HANDLER(fdiml);
  SETUP_CALL_HANDLER(fmaxf);
  SETUP_CALL_HANDLER(fmaxl);
  SETUP_CALL_HANDLER(fminf);
  SETUP_CALL_HANDLER(fminl);
  SETUP_CALL_HANDLER(fmaf);
  SETUP_CALL_HANDLER(fmal);
  SETUP_CALL_HANDLER(fmodf);
  SETUP_CALL_HANDLER(fmodl);
  SETUP_CALL_HANDLER(remainder);
  SETUP_CALL_HANDLER(remainderf);
  SETUP_CALL_HANDLER(remainderl);
  SETUP_CALL_HANDLER(log10f);
  SETUP_CALL_HANDLER(log10l);
  SETUP_CALL_HANDLER(log1pf);
  SETUP_CALL_HANDLER(log1pl);
  SETUP_CALL_HANDLER(log2f);
  SETUP_CALL_HANDLER(log2l);
  SETUP_CALL_HANDLER(nanf);
  SETUP_CALL_HANDLER(nanl);
  SETUP_CALL_HANDLER(sincosl);
  SETUP_CALL_HANDLER(__fpclassifyd);
  SETUP_CALL_HANDLER(__fpclassifyf);
  SETUP_CALL_HANDLER(__fpclassifyl);
  SETUP_CALL_HANDLER(timelocal);
  SETUP_CALL_HANDLER(strftime_l);
  SETUP_CALL_HANDLER(strptime_l);
  SETUP_CALL_HANDLER(gnu_dev_makedev);
  SETUP_CALL_HANDLER(gnu_dev_major);
  SETUP_CALL_HANDLER(gnu_dev_minor);
  SETUP_CALL_HANDLER(sigprocmask);
  SETUP_CALL_HANDLER(killpg);
  SETUP_CALL_HANDLER(waitid);
  SETUP_CALL_HANDLER(waitpid);
  SETUP_CALL_HANDLER(wait3);
  SETUP_CALL_HANDLER(wait4);
  SETUP_CALL_HANDLER(__errno);
  SETUP_CALL_HANDLER(__01getrlimit64_);
  SETUP_CALL_HANDLER(ntohl);
  SETUP_CALL_HANDLER(ntohs);
  SETUP_CALL_HANDLER(SDL_LoadBMP);
  SETUP_CALL_HANDLER(SDL_LoadBMP_RW);
  SETUP_CALL_HANDLER(Mix_CloseAudio);
  SETUP_CALL_HANDLER(Mix_PlayChannelTimed);
  SETUP_CALL_HANDLER(Mix_LoadMUS_RW);
  SETUP_CALL_HANDLER(Mix_FreeMusic);
  SETUP_CALL_HANDLER(Mix_FadeInMusicPos);
  SETUP_CALL_HANDLER(Mix_FadeOutMusic);
  SETUP_CALL_HANDLER(TTF_RenderText_Blended);
  SETUP_CALL_HANDLER(TTF_RenderText_Shaded);
  SETUP_CALL_HANDLER(TTF_RenderUTF8_Solid);
  SETUP_CALL_HANDLER(SDL_getenv);
  SETUP_CALL_HANDLER(SDL_RWFromMem);
}

std::string handleCall(const CallInst *CI) {
  const Value *CV = CI->getCalledValue();
  assert(!isa<InlineAsm>(CV) && "asm() not supported, use EM_ASM() (see emscripten.h)");
  std::string Name = getJSName(CV);
  if (strcmp(Name.c_str(), "_llvm_dbg_value") == 0) return ""; // ignore this
  unsigned NumArgs = CI->getNumArgOperands();
  CallHandlerMap::iterator CH = CallHandlers->find("___default__");
  if (isa<Function>(CV)) {
    CallHandlerMap::iterator Custom = CallHandlers->find(Name);
    if (Custom != CallHandlers->end()) CH = Custom;
  }
  return (this->*(CH->second))(CI, Name, NumArgs);
}

