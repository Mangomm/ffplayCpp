/*
时间：2020/12/25.
功能：
此单例模板为懒汉，所以已经确保了为单例,并且不需要担心释放问题。
此后若继承的话，并增加了额外数据，可使用锁进行保护。
作者：tyy.

用法：可以直接存放一个已有的对象使用或者继承使用，建议后者.
1）Instance<MyClass>::GetInstance();MyClass为自己声明已有的类.不建议使用系统的类(string，map等)，可能出现未知问题。
2）继承用法。

建议：不建议写出饿汉和使用shared_ptr()搭载make_shared()，容易出问题.
饿汉->编译时生成导致继承的类的构造析构不能为私有,保护,只能为公有，所以模板弃掉饿汉。
shared_ptr()搭载make_shared()->make_shared()new对象时要求为std::forward<_Args>(__args)...多参列表,不能只传new T。std::shared_ptr<T>(new T);可以解决.

所以单例的模板建议使用饿汉和指针。
理解时：建议用模板二次编译生成何种类去理解,用继承理解也行，不过比普通继承的理解稍难点。
*/

#ifndef __INSTANCE__H__
#define __INSTANCE__H__
#include <iostream>
#include <mutex>

namespace N1 {
    namespace N2 {
        namespace N3 {
            template<typename T>
            class Instance {
            public:
                static T* GetInstance() {
                    if (m_instance == NULL) {
                        std::unique_lock<std::mutex> uLock(m_mutex);
                        if (m_instance == NULL) {
                            m_instance = new T();
                            static Garbor gar;
                        }
                    }
                    return m_instance;
                }
            public:
                void InStanceLock() {
                    m_mutex_self.lock();
                }
                void InstanceUnlock() {
                    m_mutex_self.unlock();
                }


                //模板基类需要是保护，保证new子类时可以继承调用.
            protected:
                explicit Instance() {/*std::cout << "create" << std::endl;*/ }
                virtual ~Instance() {/*std::cout << "destory" << std::endl;*/ }//自动析构
                Instance(const T *other) = delete;//Instance(const std::shared_ptr<T> &other) = delete;
                T* operator=(const T *other) = delete;//std::shared_ptr<T>& operator=(const std::shared_ptr<T> &other) = delete;

            private:
                static T *m_instance;//static std::shared_ptr<T> m_instance;
                static std::mutex m_mutex;//锁需要静态,因为懒汉对象创建之前需要上锁确保多线程安全.
                static std::mutex m_mutex_self;//锁住单例本身，否则多线程同时拿到单例，同时使用，仍会报错
                class Garbor {
                public:
                    Garbor() {}
                    ~Garbor() { if (m_instance) { delete m_instance; m_instance = NULL; } }
                };
            };

            //模板单例不能使用饿汉。
            //静态类的类外初始化(必须加template声明),饿汉模式
            // template<typename T>
            // std::shared_ptr<T> Instance<T>::m_instance = std::shared_ptr<T>(new T);

            template<typename T>
            T* Instance<T>::m_instance = NULL;//要先定义为空,否则无法使用判断是否为空.
            template<typename T>
            std::mutex Instance<T>::m_mutex;
            template<typename T>
            std::mutex Instance<T>::m_mutex_self;
        }
    }
}

#endif
