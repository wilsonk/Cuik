

/*#define hash_hash # ## #
#define mkstr(a) # a
#define in_between(a) mkstr(a)
#define join(c, d) in_between(c hash_hash d)

// join(x, y)
// in_between(x hash_hash y)
// in_between(x ## y)
// mkstr(x ## y)
// "x ## y"
char p[] = join(x, y);

#define foo(a, ...) (a + __VA_ARGS__)

foo(x,y,z);*/

/*#define x 3
#define f(a) f(x * (a))
#undef x
#define x 2
#define g f
#define z z[0]
#define h g(~
    #define m(a) a(w)
    #define w 0,1
    #define t(a) a
    #define p() int
    #define q(x) x
    #define r(x,y) x ## y
    #define str(x) # x
    f(y+1) + f(f(z)) % t(t(g)(0) + t)(1);
    g(x+(3,4)-w) | h 5) & m
(f)^m(m);
p() i[q()] = { q(1), r(2,3), r(4,), r(,5), r(,) };
char c[2][6] = { str(hello), str() };*/

#include <stdio.h>

int main() { return printf("Hello, World!"); }