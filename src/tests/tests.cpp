#include "CppUnitTest.h"

#pragma comment(lib, "C:/Users/User/Downloads/The-Simpsons-Hit-Run/src/tests/lib/Microsoft.VisualStudio.TestTools.CppUnitTestFramework.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
    TEST_CLASS(SmokeTests)
    {
    public:
        TEST_METHOD(BasicMath)
        {
            Assert::AreEqual(2, 1 + 1);
        }
    };
}
