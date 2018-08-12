/*
 * Copyright (c) 2018 https://www.thecoderscorner.com (Nutricherry LTD).
 * This product is licensed under an Apache license, see the LICENSE file in the top-level directory.
 */

#include <inttypes.h>
#include <Arduino.h>
#include "TaskManager.h"

TaskManager taskManager;

TimerTask::TimerTask() {
	next = NULL;
	clear();
}

void TimerTask::initialise(uint16_t executionInfo, TimerFn execCallback) {
	this->executionInfo = executionInfo;
	this->callback = execCallback;
	
	this->scheduledAt = (executionInfo & TASK_MICROS) ? micros() : millis();
}

bool TimerTask::isReady() {
	if (!isInUse() || isRunning()) return false;

	if ((executionInfo & TASK_MICROS) != 0) {
		uint16_t delay = (executionInfo & TIMER_MASK);
		return (micros() - scheduledAt) >= delay;
	}
	else if((executionInfo & TASK_SECONDS) != 0) {
		uint32_t delay = (executionInfo & TIMER_MASK) * 1000L;
		return (millis() - scheduledAt) >= delay;
	}
	else {
		uint16_t delay = (executionInfo & TIMER_MASK);
		return (millis() - scheduledAt) >= delay;
	}
}

unsigned long TimerTask::microsFromNow() {
	uint32_t microsFromNow;
	if ((executionInfo & TASK_MICROS) != 0) {
		uint16_t delay = (executionInfo & TIMER_MASK);
		microsFromNow =  delay - (micros() - scheduledAt);
	}
	else {
		uint32_t startTm = (executionInfo & TIMER_MASK);
		if ((executionInfo & TASK_SECONDS) != 0) {
			startTm *= 1000;
		}
		microsFromNow = (startTm - (millis() - scheduledAt)) * 1000;
	}
	return microsFromNow;
}


inline void TimerTask::execute() {
	if (callback == NULL) return; // failsafe, always exit with null callback.

	// handle repeating tasks - reschedule.
	if (isRepeating()) {
		markRunning();
		callback();
		this->scheduledAt = (executionInfo & TASK_MICROS) ? micros() : millis();
		clearRunning();
	}
	else {
		TimerFn savedCallback = callback;
		clear();
		savedCallback();
	}
}

void TimerTask::clear() {
	executionInfo = 0;
	callback = NULL;
}

void TaskManager::markInterrupted(uint8_t interruptNo) {
	taskManager.lastInterruptTrigger = interruptNo;
	taskManager.interrupted = true;
}

TaskManager::TaskManager(uint8_t taskSlots) {
	this->numberOfSlots = taskSlots;
	this->tasks = new TimerTask[taskSlots];
	interrupted = false;
	first = NULL;
	interruptCallback = NULL;
}

int TaskManager::findFreeTask() {
	for (uint8_t i = 0; i < numberOfSlots; ++i) {
		if (!tasks[i].isInUse()) {
			return i;
		}
	}
	return TASKMGR_INVALIDID;
}

inline int toTimerValue(int v, TimerUnit unit) {
	if (unit == TIME_MILLIS && v > TIMER_MASK) {
		unit = TIME_SECONDS;
		v = v / 1000;
	}
	v = min(v, TIMER_MASK);
	return v | (((uint16_t)unit) << 12);
}

uint8_t TaskManager::scheduleOnce(int millis, TimerFn timerFunction, TimerUnit timeUnit) {
	uint8_t taskId = findFreeTask();
	if (taskId != TASKMGR_INVALIDID) {
		tasks[taskId].initialise(toTimerValue(millis, timeUnit) | TASK_IN_USE, timerFunction);
		putItemIntoQueue(&tasks[taskId]);
	}
	return taskId;
}

uint8_t TaskManager::scheduleFixedRate(int millis, TimerFn timerFunction, TimerUnit timeUnit) {
	uint8_t taskId = findFreeTask();
	if (taskId != TASKMGR_INVALIDID) {
		tasks[taskId].initialise(toTimerValue(millis, timeUnit) | TASK_IN_USE | TASK_REPEATING, timerFunction);
		putItemIntoQueue(&tasks[taskId]);
	}
	return taskId;
}

void TaskManager::cancelTask(uint8_t task) {
	if (task < numberOfSlots) {
		tasks[task].clear();
		removeFromQueue(&tasks[task]);
	}
}

char* TaskManager::checkAvailableSlots(char* data) {
	uint8_t i;
	for (i = 0; i < numberOfSlots; ++i) {
		data[i] = tasks[i].isRepeating() ? 'R' : (tasks[i].isInUse() ? 'U' : 'F');
		if (tasks[i].isRunning()) data[i] = tolower(data[i]);
	}
	data[i] = 0;
	return data;
}

void TaskManager::yieldForMicros(uint16_t microsToWait) {
	yield();
	
	unsigned long microsEnd = micros() + microsToWait;
	while(micros() < microsEnd) {
		runLoop();
	}
}

void TaskManager::runLoop() {
	if (interrupted) {
		interrupted = false;
		interruptCallback(lastInterruptTrigger);
	}

	TimerTask* tm = first;
	while(tm != NULL) {
		if (tm->isReady()) {
			removeFromQueue(tm);
			tm->execute();
			if (tm->isRepeating()) {
				putItemIntoQueue(tm);
			}
		}
		else {
			break;
		}

		tm = tm->getNext();
	}

	// TODO, if not interrupted, and no task for a while sleep and set timer - go into low power.
}

void TaskManager::putItemIntoQueue(TimerTask* tm) {

	// shortcut, no first yet, so we are at the top!
	if (first == NULL) {
		first = tm;
		tm->setNext(NULL);
		return;
	}

	// if we are the new first..
	if (first->microsFromNow() > tm->microsFromNow()) {
		tm->setNext(first);
		first = tm;
		return;
	}

	// otherwise we have to find the place in the queue for this item by time
	TimerTask* current = first->getNext();
	TimerTask* previous = first;

	while (current != NULL) {
		if (current->microsFromNow() > tm->microsFromNow()) {
			previous->setNext(tm);
			tm->setNext(current);
			return;
		}
		previous = current;
		current = current->getNext();
	}

	// we are at the end of the queue
	previous->setNext(tm);
	tm->setNext(NULL);
}

void TaskManager::removeFromQueue(TimerTask* tm) {
	
	// shortcut, if we are first, just remove us by getting the next and setting first.
	if (first == tm) {
		first = tm->getNext();
		return;
	}

	// otherwise, we have a single linked list, so we need to keep previous and current and
	// then iterate through each item
	TimerTask* current = first->getNext();
	TimerTask* previous = first;

	while (current != NULL) {

		// we've found the item, unlink it from the queue and nullify its next.
		if (current == tm) {
			previous->setNext(current->getNext());
			current->setNext(NULL);
			break;
		}

		previous = current;
		current = current->getNext();
	}
}

typedef void ArdunioIntFn(void);

void interruptHandler1() {
	taskManager.markInterrupted(1);
}
void interruptHandler2() {
	taskManager.markInterrupted(2);
}
void interruptHandler3() {
	taskManager.markInterrupted(3);
}
void interruptHandler5() {
	taskManager.markInterrupted(5);
}
void interruptHandler18() {
	taskManager.markInterrupted(18);
}
void interruptHandlerOther() {
	taskManager.markInterrupted(0xff);
}

void TaskManager::addInterrupt(uint8_t pin, uint8_t mode) {
	if (interruptCallback == NULL) return;
	int interruptNo = digitalPinToInterrupt(pin);

	switch (pin) {
	case 1:  attachInterrupt(interruptNo, interruptHandler1, mode); break;
	case 2: attachInterrupt(interruptNo, interruptHandler2, mode); break;
	case 3: attachInterrupt(interruptNo, interruptHandler3, mode); break;
	case 5: attachInterrupt(interruptNo, interruptHandler5, mode); break;
	case 18: attachInterrupt(interruptNo, interruptHandler18, mode); break;
	default: attachInterrupt(interruptNo, interruptHandlerOther, mode); break;
	}
}

void TaskManager::setInterruptCallback(InterruptFn handler) {
	interruptCallback = handler;
}

