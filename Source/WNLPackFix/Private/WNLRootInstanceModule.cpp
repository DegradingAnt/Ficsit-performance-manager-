#include "WNLRootInstanceModule.h"
#include "WNLModConfiguration.h"

UWNLRootInstanceModule::UWNLRootInstanceModule()
{
	// SML finds the one root module per mod by this flag (class name is free; kept descriptive).
	bRootModule = true;

	// Declarative registration (the documented pattern): SML registers this configuration with the
	// ConfigManager at the right lifecycle moment — no manual subsystem lookup needed.
	ModConfigurations.Add(UWNLModConfiguration::StaticClass());
}
