#ifndef _TESTWIFITASK_H
#define _TESTWIFITEASK_H

#include "network.h"
#include "ntptask.h"

class CTestWifiTask:public CNTPTask {
public:
  CTestWifiTask(TestResult_t *p):CNTPTask() {this->testResultPtr=p;}

protected:
  TestResult_t *testResultPtr;

  //Override base class methods
  void EvtStart();
};

#endif

