/* Demonstrate that a single lambda can cause the creation of many
 * anonymous classes. In OpenJDK 1.8.0_121, uncommenting framents A
 * and B leads to 72 anonymous classes being dynamically
 * generated. Otherwise, no anonymous classes are generated.
 */

import java.util.function.*;

public class Lam {
    public static void main(String[] args) {
	A a = new A();
	System.out.println(10);
	// ** Fragment A **
	// System.out.println(a.f(x -> x + 1));
    }
}

class A {
    // ** Fragment B **
    // public Integer f(IntFunction<Integer> func) {
    // 	return func.apply(10);
    // }
}
