// math-test: libm (M2 step 5c). Built with -fno-builtin, so every call reaches libm.so instead of
// being folded by the compiler.
//   - Functions IEEE 754 defines exactly (sqrt, fma, rounding, remainders, scaling) print their bits
//     as hex floats: Android's libm must give exactly these values.
//   - The others print 14 significant digits (floats 6): each value is at least 4 ulp away from
//     the next rounding boundary, so every accurate libm prints the same.
//   - Exceptions and rounding modes through fenv.h; bionic's math functions report errors through
//     the exception flags only (math_errhandling is MATH_ERREXCEPT), errno stays as it was.
// Expected output: fixtures/expected/math-test.out
#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <stdio.h>

#define EXACT(expression) printf("%-30s %a\n", #expression, (double)(expression))
#define EXACTF(expression) printf("%-30s %a\n", #expression, (double)(expression))
#define NEAR(expression) printf("%-30s %.14g\n", #expression, (double)(expression))
#define NEARF(expression) printf("%-30s %.6g\n", #expression, (double)(expression))
#define INTEGER(expression) printf("%-30s %lld\n", #expression, (long long)(expression))

static void exact(void) {
    EXACT(sqrt(2.0));
    EXACT(sqrt(1e-300));
    EXACT(fma(0.1, 10.0, -1.0));
    EXACT(fma(3.0, 5.0, 0.25));
    EXACT(floor(-2.5));
    EXACT(ceil(-2.5));
    EXACT(trunc(-2.7));
    EXACT(round(2.5));
    EXACT(round(-2.5));
    EXACT(rint(2.5));
    EXACT(rint(3.5));
    EXACT(nearbyint(-0.5));
    INTEGER(lround(-3.5));
    INTEGER(llrint(4.5));
    EXACT(fmod(10.3, 3.0));
    EXACT(remainder(10.3, 3.0));
    EXACT(ldexp(0.75, 10));
    EXACT(scalbn(1.5, -3));
    INTEGER(ilogb(1234.5));
    EXACT(logb(1234.5));
    EXACT(copysign(3.0, -0.0));
    EXACT(fabs(-7.25));
    EXACT(fmin(1.0, NAN));
    EXACT(fmax(-3.0, 2.0));
    EXACT(fdim(5.0, 3.5));
    EXACT(nextafter(1.0, 2.0));
    EXACT(nextafter(1.0, 0.0));
    EXACT(hypot(3.0, 4.0));
    EXACTF(sqrtf(2.0f));
    EXACTF(fmaf(0.1f, 10.0f, -1.0f));
    EXACTF(floorf(-0.5f));

    int exponent = 0;
    const double mantissa = frexp(1234.5, &exponent);
    printf("%-30s %a, %d\n", "frexp(1234.5)", mantissa, exponent);
    double whole = 0;
    const double fraction = modf(-3.25, &whole);
    printf("%-30s %a, %a\n", "modf(-3.25)", fraction, whole);
    int quotient = 0;
    const double rest = remquo(10.3, 3.0, &quotient);
    printf("%-30s %a, %d\n", "remquo(10.3, 3.0)", rest, quotient);

    volatile long double two = 2;
    printf("%-30s %La\n", "sqrtl(2.0L)", sqrtl(two));
    printf("%-30s %.30Lg\n", "expl(1.0L)", expl(two / 2));
}

static void near(void) {
    NEAR(exp(1.0));
    NEAR(exp(-10.5));
    NEAR(expm1(1e-5));
    NEAR(exp2(0.5));
    NEAR(log(10.0));
    NEAR(log(2.5e-8));
    NEAR(log2(10.0));
    NEAR(log10(54321.0));
    NEAR(log1p(1e-4));
    NEAR(pow(2.0, 0.5));
    NEAR(pow(10.0, -3.3));
    NEAR(pow(1.0001, 10000.0));
    NEAR(cbrt(27.5));
    NEAR(sin(1.0));
    NEAR(cos(1.0));
    NEAR(tan(1.0));
    NEAR(sin(1e6));
    NEAR(cos(1e22));
    NEAR(asin(0.5));
    NEAR(acos(-0.3));
    NEAR(atan(2.0));
    NEAR(atan2(-1.0, -2.0));
    NEAR(sinh(2.0));
    NEAR(cosh(2.0));
    NEAR(tanh(0.5));
    NEAR(asinh(1.0));
    NEAR(acosh(2.0));
    NEAR(atanh(0.25));
    NEAR(erf(0.5));
    NEAR(erfc(2.0));
    NEAR(tgamma(4.5));
    NEAR(lgamma(10.5));
    NEARF(expf(1.0f));
    NEARF(logf(3.0f));
    NEARF(powf(2.0f, 0.5f));
    NEARF(sinf(1.0f));
    NEARF(cosf(2.0f));
    NEARF(sinf(1e5f));
    NEARF(atan2f(2.0f, 5.0f));
}

static void report(const char *what, double value, int flags) {
    printf("%-30s %s%s, flags:%s%s%s%s%s, errno %d\n", what, isnan(value) ? "nan" : "",
           isnan(value) ? "" : isinf(value) ? (value < 0 ? "-inf" : "inf") : "finite",
           (flags & FE_INVALID) ? " invalid" : "", (flags & FE_DIVBYZERO) ? " divbyzero" : "",
           (flags & FE_OVERFLOW) ? " overflow" : "", (flags & FE_UNDERFLOW) ? " underflow" : "",
           (flags & FE_INEXACT) ? " inexact" : "", errno);
}

#define SPECIAL(expression)                                                        \
    do {                                                                           \
        errno = 0;                                                                 \
        feclearexcept(FE_ALL_EXCEPT);                                              \
        const double value = (expression);                                         \
        report(#expression, value, fetestexcept(FE_ALL_EXCEPT));                   \
    } while (0)

static volatile double one = 1.0, three = 3.0;

static void special(void) {
    SPECIAL(log(0.0));
    SPECIAL(log(-1.0));
    SPECIAL(sqrt(-1.0));
    SPECIAL(exp(1000.0));
    SPECIAL(exp(-1000.0));
    SPECIAL(pow(0.0, -1.0));
    SPECIAL(one / three);
    SPECIAL(atan2(0.0, -0.0));
    EXACT(atan2(0.0, -0.0));
    printf("%-30s %d %d %d %d\n", "fpclassify", fpclassify(0.0) == FP_ZERO, fpclassify(1e-310) == FP_SUBNORMAL,
           fpclassify(INFINITY) == FP_INFINITE, fpclassify(1.0) == FP_NORMAL);
    printf("%-30s %d %d\n", "signbit(-0.0), signbit(0.0)", signbit(-0.0) != 0, signbit(0.0) != 0);
}

static void rounding_modes(void) {
    const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    const char *names[] = {"to nearest", "upward", "downward", "toward zero"};
    for (int i = 0; i < 4; ++i) {
        fesetround(modes[i]);
        const int current = fegetround();
        printf("%-30s rint(2.5) %a, rint(-2.5) %a, lrint(-2.7) %ld, same mode %d\n", names[i], rint(2.5),
               rint(-2.5), lrint(-2.7), current == modes[i]);
    }
    fesetround(FE_TONEAREST);
}

int main(void) {
    exact();
    near();
    special();
    rounding_modes();
    return 0;
}
