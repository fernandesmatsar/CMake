/*=========================================================================

  Program:   CMake - Cross-Platform Makefile Generator
  Module:    $RCSfile$
  Language:  C++
  Date:      $Date$
  Version:   $Revision$

  Copyright (c) 2002 Kitware, Inc., Insight Consortium.  All rights reserved.
  See Copyright.txt or http://www.cmake.org/HTML/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#include "cmCTestMultiProcessHandler.h"
#include "cmProcess.h"
#include "cmStandardIncludes.h"
#include "cmCTest.h"
#include "cmSystemTools.h"
#include <stdlib.h>

cmCTestMultiProcessHandler::cmCTestMultiProcessHandler()
{
  this->ParallelLevel = 1;
  this->Completed = 0;
  this->RunningCount = 0;
}

cmCTestMultiProcessHandler::~cmCTestMultiProcessHandler()
{
}

  // Set the tests
void 
cmCTestMultiProcessHandler::SetTests(TestMap& tests,
                                     PropertiesMap& properties)
{
  this->Tests = tests;
  this->Properties = properties;
  this->Total = this->Tests.size();
  // set test run map to false for all
  for(TestMap::iterator i = this->Tests.begin();
      i != this->Tests.end(); ++i)
    {
    this->TestRunningMap[i->first] = false;
    this->TestFinishMap[i->first] = false;
    }
  this->ReadCostData();
  this->CreateTestCostList();
}

  // Set the max number of tests that can be run at the same time.
void cmCTestMultiProcessHandler::SetParallelLevel(size_t level)
{
  this->ParallelLevel = level < 1 ? 1 : level;
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::RunTests()
{
  this->CheckResume();
  this->TestHandler->SetMaxIndex(this->FindMaxIndex());
  this->StartNextTests();
  while(this->Tests.size() != 0)
    {
    this->CheckOutput();
    this->StartNextTests();
    }
  // let all running tests finish
  while(this->CheckOutput())
    {
    }
  this->MarkFinished();
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::StartTestProcess(int test)
{
  cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, "test " << test << "\n");
  this->TestRunningMap[test] = true; // mark the test as running
  // now remove the test itself
  this->EraseTest(test);

  cmCTestRunTest* testRun = new cmCTestRunTest(this->TestHandler);
  testRun->SetIndex(test);
  testRun->SetTestProperties(this->Properties[test]);
  if(testRun->StartTest())
    {
    this->RunningTests.insert(testRun);
    }
  else
    {
    this->Completed++;
    this->RunningCount -= GetProcessorsUsed(test);
    testRun->EndTest(this->Completed, this->Total, false);
    }
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::EraseTest(int test)
{
  this->Tests.erase(test);
  for(TestCostMap::iterator i = this->TestCosts.begin();
      i != this->TestCosts.end(); ++i)
    {
    if(i->second.find(test) != i->second.end())
      {
      i->second.erase(test);
      return;
      }
    }
}

//---------------------------------------------------------
inline size_t cmCTestMultiProcessHandler::GetProcessorsUsed(int test)
{
  size_t processors = 
    static_cast<int>(this->Properties[test]->Processors);
  //If this is set to run serially, it must run alone.
  //Also, if processors setting is set higher than the -j
  //setting, we default to using all of the process slots.
  if(this->Properties[test]->RunSerial
     || processors > this->ParallelLevel)
    {
    processors = this->ParallelLevel;
    }
  return processors;
}

//---------------------------------------------------------
bool cmCTestMultiProcessHandler::StartTest(int test)
{
  // copy the depend tests locally because when 
  // a test is finished it will be removed from the depend list
  // and we don't want to be iterating a list while removing from it
  TestSet depends = this->Tests[test];
  size_t totalDepends = depends.size();
  if(totalDepends)
    {
    for(TestSet::const_iterator i = depends.begin();
        i != depends.end(); ++i)
      {
      // if the test is not already running then start it
      if(!this->TestRunningMap[*i])
        {
        // this test might be finished, but since
        // this is a copy of the depend map we might
        // still have it
        if(!this->TestFinishMap[*i])
          {
          // only start one test in this function
          return this->StartTest(*i);
          }
        else
          {
          // the depend has been and finished
          totalDepends--;
          }
        }
      }
    }
  // if there are no depends left then run this test
  if(totalDepends == 0)
    {
    this->StartTestProcess(test);
    return true;
    }
  // This test was not able to start because it is waiting 
  // on depends to run
  return false;
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::StartNextTests()
{
  size_t numToStart = this->ParallelLevel - this->RunningCount;
  if(numToStart == 0)
    {
    return;
    }

  for(TestCostMap::reverse_iterator i = this->TestCosts.rbegin();
      i != this->TestCosts.rend(); ++i)
    {
    TestSet tests = i->second; //copy the test set
    for(TestSet::iterator test = tests.begin();
        test != tests.end(); ++test)
      {
      size_t processors = GetProcessorsUsed(*test);
      if(processors > numToStart)
        {
        return;
        }
      if(this->StartTest(*test))
        {
        numToStart -= processors;
        this->RunningCount += processors;
        }
      else
        {
        cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, std::endl
                   << "Test did not start waiting on depends to finish: "
                   << *test << "\n");
        }
      if(numToStart == 0)
        {
        return;
        }
      }
    }
}

//---------------------------------------------------------
bool cmCTestMultiProcessHandler::CheckOutput()
{
  // no more output we are done
  if(this->RunningTests.size() == 0)
    {
    return false;
    }
  std::vector<cmCTestRunTest*> finished;
  std::string out, err;
  for(std::set<cmCTestRunTest*>::const_iterator i = this->RunningTests.begin();
      i != this->RunningTests.end(); ++i)
    {
    cmCTestRunTest* p = *i;
    p->CheckOutput(); //reads and stores the process output
    
    if(!p->IsRunning())
      {
      finished.push_back(p);
      }
    }
  for( std::vector<cmCTestRunTest*>::iterator i = finished.begin();
       i != finished.end(); ++i)
    {
    this->Completed++;
    cmCTestRunTest* p = *i;
    int test = p->GetIndex();

    if(p->EndTest(this->Completed, this->Total, true))
      {
      this->Passed->push_back(p->GetTestProperties()->Name);
      }
    else
      {
      this->Failed->push_back(p->GetTestProperties()->Name);
      }
    for(TestMap::iterator j = this->Tests.begin();
        j != this->Tests.end(); ++j)
      {
      j->second.erase(test);
      }
    this->TestFinishMap[test] = true;
    this->TestRunningMap[test] = false;
    this->RunningTests.erase(p);
    this->WriteCheckpoint(test);
    this->WriteCostData(test, p->GetTestResults().ExecutionTime);
    this->RunningCount -= GetProcessorsUsed(test);
    delete p;
    }
  return true;
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::ReadCostData()
{
  std::string fname = this->CTest->GetBinaryDir()
    + "/Testing/Temporary/CTestCostData.txt";

  if(cmSystemTools::FileExists(fname.c_str(), true)
     && this->ParallelLevel > 1)
    {       
    std::ifstream fin;
    fin.open(fname.c_str());
    std::string line;
    while(std::getline(fin, line))
      {
      std::vector<cmsys::String> parts = 
        cmSystemTools::SplitString(line.c_str(), ' ');

      int index = atoi(parts[0].c_str());
      float cost = atof(parts[1].c_str());
      if(this->Properties[index] && this->Properties[index]->Cost == 0)
        {
        this->Properties[index]->Cost = cost;
        }
      }
    fin.close();
    }
  cmSystemTools::RemoveFile(fname.c_str());
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::CreateTestCostList()
{
  for(TestMap::iterator i = this->Tests.begin();
      i != this->Tests.end(); ++i)
    {
    this->TestCosts[this->Properties[i->first]->Cost].insert(i->first);
    }
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::WriteCostData(int index, float cost)
{
  std::string fname = this->CTest->GetBinaryDir()
    + "/Testing/Temporary/CTestCostData.txt";
  std::fstream fout;
  fout.open(fname.c_str(), std::ios::app);
  fout << index << " " << cost << "\n";
  fout.close();
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::WriteCheckpoint(int index)
{
  std::string fname = this->CTest->GetBinaryDir()
    + "/Testing/Temporary/CTestCheckpoint.txt";
  std::fstream fout;
  fout.open(fname.c_str(), std::ios::app);
  fout << index << "\n";
  fout.close();
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::MarkFinished()
{
  std::string fname = this->CTest->GetBinaryDir()
    + "/Testing/Temporary/CTestCheckpoint.txt";
  cmSystemTools::RemoveFile(fname.c_str());
}

//---------------------------------------------------------
//For ShowOnly mode
void cmCTestMultiProcessHandler::PrintTestList()
{
  int count = 0;
  for (PropertiesMap::iterator it = this->Properties.begin();
       it != this->Properties.end(); it ++ )
    {
    count++;
    cmCTestTestHandler::cmCTestTestProperties& p = *it->second;

    cmCTestRunTest testRun(this->TestHandler);
    testRun.SetIndex(p.Index);
    testRun.SetTestProperties(&p);
    testRun.ComputeArguments(); //logs the command in verbose mode

    cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(3)
             << count << "/");
    cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(3)
             << this->Total << " ");
    if (this->TestHandler->MemCheck)
      {
      cmCTestLog(this->CTest, HANDLER_OUTPUT, "Memory Check");
      }
     else
      {
      cmCTestLog(this->CTest, HANDLER_OUTPUT, "Testing");
      }
    cmCTestLog(this->CTest, HANDLER_OUTPUT, " ");
    cmCTestLog(this->CTest, HANDLER_OUTPUT, p.Name.c_str() << std::endl);
    }
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::CheckResume()
{
  std::string fname = this->CTest->GetBinaryDir()
      + "/Testing/Temporary/CTestCheckpoint.txt";
  if(this->CTest->GetFailover())
    {
    if(cmSystemTools::FileExists(fname.c_str(), true))
      {
      *this->TestHandler->LogFile << "Resuming previously interrupted test set"
        << std::endl
        << "----------------------------------------------------------"
        << std::endl;
        
      std::ifstream fin;
      fin.open(fname.c_str());
      std::string line;
      while(std::getline(fin, line))
        {
        int index = atoi(line.c_str());
        this->RemoveTest(index);
        }
      fin.close();
      }
    }
  else
    {
    if(cmSystemTools::FileExists(fname.c_str(), true))
      {
      cmSystemTools::RemoveFile(fname.c_str());
      }
    }
}

//---------------------------------------------------------
void cmCTestMultiProcessHandler::RemoveTest(int index)
{
  this->EraseTest(index);
  this->Properties.erase(index);
  this->TestRunningMap[index] = false;
  this->TestFinishMap[index] = true;
  this->Completed++;
}

//---------------------------------------------------------
int cmCTestMultiProcessHandler::FindMaxIndex()
{
  int max = 0;
  cmCTestMultiProcessHandler::TestMap::iterator i = this->Tests.begin();
  for(; i != this->Tests.end(); ++i)
    {
    if(i->first > max)
      {
      max = i->first;
      }
    }
  return max;
}
