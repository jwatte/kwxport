utility kwutils "kW Utilities"
(
	global lastName = undefined;
	button export "Export..." width:120;
	button exportLast "Export Last" width:120;
	button settings "Settings..." width:120;
	button saveSettings "Save Settings..." width:120;
	button loadSettings "Load Settings..." width:120;
	checkbox includeAnimation "Include Animation" align:#center;
	on export pressed do
	(
		newName = getSaveFileName caption:"Save X file" filename:lastName types:".X file (*.x)|*.x";
		if newName != undefined then
		(
			kwfunctions.export newName;
			lastName = newName;
		)
	)
	on exportLast pressed do
	(
		if lastName == undefined then
		(
			lastName = getSaveFileName caption:"Save X file" filename:lastName types:".X file (*.x)|*.x";
		)
		kwfunctions.export lastName;
	)
	on saveSettings pressed do
	(
		local sfn = getSaveFileName caption:"Save Settings" filename:"settings.txt" types:"text files (*.txt)|*.txt";
		if sfn != undefined then
		(
			kwfunctions.savesettings sfn;
		)
	)
	on loadSettings pressed do
	(
		local sfn = getOpenFileName caption:"Open Settings" filename:"settings.txt" types:"text files (*.txt)|*.txt";
		if sfn != undefined then
		(
			ia = includeAnimation.checked;
			kwfunctions.loadsettings sfn ia;
		)
	)
	on settings pressed do
	(
		kwfunctions.showsettings();
	)
)
