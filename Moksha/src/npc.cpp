#include "npc.h"

NPC::NPC(Mapa* m, string name, int gender, int strength, int dexterity) : Personagem(gender, strength, dexterity) {
	this->name = name;
	this->mapa = m;

	FileDict fileObject = FileManager::readFromFile("files/characters/" + getName() + ".txt");
	this->dict = fileObject;
}


// PATHFINDING ---------------

queue<Sala*> NPC::findPath(Sala* salaAlvo) {
	return findPath(currentRoom, salaAlvo);
};

queue<Sala*> NPC::findPath(Sala* salaInicial, Sala* salaAlvo) {
	return mapa->optimalPath(salaInicial, salaAlvo);
};

queue<Sala*> NPC::search() {
	return search(currentRoom);
};

queue<Sala*> NPC::search(Sala* roomClue) {
	// Path to the clue
	queue<Sala*> retorno;
	retorno.push(currentRoom);
	if (roomClue != currentRoom)
		retorno = findPath(roomClue);

	// Start search
	queue<Sala*> search = mapa->breadthSearch(roomClue);
	while (!search.empty()) {
		queue<Sala*> path = findPath(retorno.back(), search.front());
		while (!path.empty()) {
			retorno.push(path.front());
			path.pop();
		}
		search.pop();
	}

	if (retorno.front() == currentRoom)
		retorno.pop();
	return retorno;
};


string NPC::nextRoomInPath() {
	string retorno = "";
	if (path.empty())
		advancePlansExtra(plan[currentStep]); // Refresh plans

	if (!path.empty()) {
		retorno = path.front()->getName();
		path.pop();
	}

	return retorno;
}

// REACTIONS ----------------------------------

void NPC::executeReaction(string topic, string phrase, string sender, bool shouldRespond) {
	if (isUnconscious())
		return;

	if (topic != "")
		setCondition(topic, true);
	if (dict.hasKey(topic) && shouldRespond)
		say(topic, dict.getValue(topic), set<string>({ sender }));
}


void NPC::setCondition(string condition, bool update) {
	if (!hasCondition(condition)) {
		string *pointer = new string(condition);
		addedConditions.insert(pointer);
		goap_worldstate_set(&ap, &world, pointer->c_str(), update);
	}
	else
		goap_worldstate_set(&ap, &world, condition.c_str(), update);
}


void NPC::receiveEvent(vector<string> args) {
	if (args[0] == "attack")
		if (trackablePeople.count(args[1]) > 0)
			setCondition(args[1] + "_dead", true);
}


void NPC::checkRoom(vector<Personagem*> peopleInRoom) {
	// Run through tracked people
	for (set<string>::iterator it = trackablePeople.begin(); it != trackablePeople.end(); it++) {
		string theirName = *it;
		goap_worldstate_set(&ap, &world, ("with_" + theirName).c_str(), false);

		// Check if they are in the room
		for (int i = 0; i < peopleInRoom.size(); i++) {
			if (theirName == peopleInRoom[i]->getName()) {
				updateLastSeen(theirName, currentRoom->getName());
				goap_worldstate_set(&ap, &world, ("with_" + theirName).c_str(), true);
				break;
			}
		}
	}

	updateWorld();
};


void NPC::seeCharMoving(Personagem* person, string otherRoom, bool entering) {
	if (trackablePeople.count(person->getName()) == 0)
		return;

	if (!entering)
		updateLastSeen(person->getName(), otherRoom);
	else
		updateLastSeen(person->getName(), currentRoom->getName());

	goap_worldstate_set(&ap, &world, ("with_" + person->getName()).c_str(), entering);
	updateWorld();
}


// PLANS -------------------------------------------

void NPC::setupPlans() {
	goap_actionplanner_clear(&ap); // initializes action planner

	// describe repertoire of actions
	setupAcoesAdicional();

	// describe current world state.
	goap_worldstate_clear(&world);
	for (set<string>::iterator it = trackablePeople.begin(); it != trackablePeople.end(); it++)
		setCondition("with_" + *it, false);
	setupMundoAdicional();

	// describe goal
	goap_worldstate_clear(&currentGoal.goal);
	setupObjetivosAdicional();
	goalList = PriorityVector<Goal>(vector<Goal>(), goalCompare);
	goalList.push(currentGoal);

	// calculate initial plan
	planCost = astar_plan(&ap, world, currentGoal.goal, plan, states, &plansz);
	currentStep = -1;
	updateWorld();
	advancePlans();
}


void NPC::updateWorld() {
	updateWorldExtra();

	// Detect that current action is impossible
	if (plansz > 0) {
		bfield_t prereqs;
		goap_get_pre(&ap, plan[currentStep], &prereqs);
		if (prereqs & world.values != prereqs)
			changePlans(true);
	}
}


void NPC::updateLastSeen(string pursueTarget, string room) {
	string alvoVec[1] = { pursueTarget };
	lastSeen.addPair(set<string>(alvoVec, alvoVec + 1), room);

	if (plansz == 0)
		return;

	string currentProcess = plan[currentStep];
	if (currentProcess.compare("search_" + pursueTarget) == 0) {
		path = search(mapa->getRoom( lastSeen.getValues(pursueTarget) ));
	}
}


void NPC::advancePlans() {
	currentStep++;

	// The plan has come to its end
	if (currentStep >= plansz && plansz > 0)
		changePlans();

	// Advance plans
	if (plansz > 0) {
		string currentProcess = plan[currentStep];

		if (currentProcess.substr(0, 7).compare("search_") == 0) {
			if (lastSeen.hasKey( currentProcess.substr(7, 10000) ))
				path = search(mapa->getRoom(lastSeen.getValues( currentProcess.substr(7, 10000) )));
			else
				path = search();

		}

		advancePlansExtra(currentProcess);
	}
}


void NPC::changePlans(bool justUpdated) {
	if (!justUpdated)
		updateWorldExtra();

	// Calculate a new plan
	planCost = astar_plan(&ap, world, currentGoal.goal, plan, states, &plansz);
	currentStep = -1;
	advancePlans();
}


// ACTIONS ------------------------------------------------

int NPC::decideAction() {
	// Check if current action was completed
	while (isCurrentStateFulfilled() && plansz > 0)
		advancePlans();

	// Current objective is top priority
	if (currentGoal.goal.values == (goalList.highest()->goal.values)) {
		// Current objective was completed - find next non-completed objective
		vector<Goal>::iterator it = goalList.highest();
		if ( ((world.values & ~currentGoal.goal.dontcare) == currentGoal.goal.values || currentStep >= plansz) && !goalList.empty() ) {
			do {
				if (it->onetime)
					goalList.getVector().erase(it);

				goalList.descend(it);
				currentGoal = *it;
				currentStep = 0;
			} while ( (world.values & ~currentGoal.goal.dontcare) == currentGoal.goal.values && it != goalList.lowest() );
			changePlans();
		}
	
	// Current objective is not top priority - find next non-completed objective
	} else {
		vector<Goal>::iterator it = goalList.highest();
		while ((world.values & ~currentGoal.goal.dontcare) == currentGoal.goal.values && it != goalList.lowest() ) {
			goalList.descend(it);
			currentGoal = *it;
			currentStep = 0;
		}
		changePlans();
	}

	// Decide action
	if (plansz > 0) {
		actionArgs.clear();
		currentAction = decidirAcaoAdicional(plan[currentStep]);
	} else
		currentAction = descansar;

	return currentAction;
}


// HELPER ----------------------------------------
#include <stdexcept>

bool NPC::isCurrentStateFulfilled() {
	bfield_t worldState = (world.values & ~states[currentStep].dontcare);
	bfield_t objState = states[currentStep].values;
	if (currentStep > 0)
		objState = states[currentStep].values & ~states[currentStep-1].values;
	bool fulfilled = ((worldState & objState) == objState);
	if (fulfilled && currentStep > 0) {
		objState = states[currentStep].values | ~states[currentStep - 1].values;
		fulfilled = fulfilled && ((worldState | objState) == objState);
	}

	return fulfilled;
}


bool NPC::hasCondition(string info) {
	bool retorno;
	if (!goap_worldstate_get(&ap, &world, info.c_str(), &retorno))
		return false;
	return retorno;
}


// GETTER
vector<string> NPC::getActionList() {
	vector<string> retorno;
	for (int i = 0; i < ap.numactions; i++)
		retorno.push_back(ap.act_names[i]);
	return retorno;
}


vector<string> NPC::getAtomList() {
	vector<string> retorno;
	for (int i = 0; i < ap.numatoms; i++)
		retorno.push_back(ap.atm_names[i]);
	return retorno;
}