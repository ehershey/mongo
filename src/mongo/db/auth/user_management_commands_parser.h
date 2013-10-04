/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class AuthorizationManager;

namespace auth {

    /**
     * Takes a command object describing an invocation of the "createUser" command on the database
     * "dbname", and returns (via the output param "parsedUserObj") a user object that can be
     * inserted into admin.system.users to create the user as described by the command object.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateCreateUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedUserObj,
                                             BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "updateUser" command on the database
     * "dbname", and returns (via the output param "parsedUpdateObj") an update specifier that can
     * be used to update the user document in admin.system.users as described by the command object,
     * as well as the user name of the user being updated (via the "parsedUserName" output param).
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateUpdateUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedUpdateObj,
                                             UserName* parsedUserName,
                                             BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of one of "grantRolesToUser",
     * "revokeRolesFromUser", "grantDelegateRolesToUser", and "revokeDelegateRolesFromUser" (which
     * command it is is specified in the "cmdName" argument), and parses out the user name of the
     * user being modified, the roles being granted or revoked, and the write concern to use.
     */
    Status parseUserRoleManipulationCommand(const BSONObj& cmdObj,
                                            const StringData& cmdName,
                                            const std::string& dbname,
                                            AuthorizationManager* authzManager,
                                            UserName* parsedUserName,
                                            vector<RoleName>* parsedRoleNames,
                                            BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "removeUser" command and parses out
     * the userName of the user to be removed and the writeConcern.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateRemoveUserCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             UserName* parsedUserName,
                                             BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "removeUsersFromDatabase" command and
     * parses out the write concern.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateRemoveUsersFromDatabaseCommand(const BSONObj& cmdObj,
                                                          const std::string& dbname,
                                                          BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "usersInfo" or "rolesInfo" commands
     * (which command it is is specified in the "cmdName" argument) and parses out a BSONElement
     * with the user/role name filter to be applied, as well as the anyDB boolean.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateInfoCommands(const BSONObj& cmdObj,
                                        const StringData& cmdName,
                                        const std::string& dbname,
                                        bool* parsedAnyDb,
                                        BSONElement* parsedNameFilter);
    /**
     * Takes a command object describing an invocation of the "createRole" command on the database
     * "dbname", and returns (via the output param "parsedRoleObj") a role object that can be
     * inserted into admin.system.roles to create the role as described by the command object.
     * Also validates the input and returns a non-ok Status if there is anything wrong.
     */
    Status parseAndValidateCreateRoleCommand(const BSONObj& cmdObj,
                                             const std::string& dbname,
                                             AuthorizationManager* authzManager,
                                             BSONObj* parsedRoleObj,
                                             BSONObj* parsedWriteConcern);

    /**
     * Takes a command object describing an invocation of the "grantPrivilegesToRole" or
     * "revokePrivilegesFromRole" commands, and parses out the role name of the
     * role being modified, the privileges being granted or revoked, and the write concern to use.
     */
    Status parseAndValidateRolePrivilegeManipulationCommands(const BSONObj& cmdObj,
                                                             const StringData& cmdName,
                                                             const std::string& dbname,
                                                             RoleName* parsedRoleName,
                                                             PrivilegeVector* parsedPrivileges,
                                                             BSONObj* parsedWriteConcern);

} // namespace auth
} // namespace mongo
