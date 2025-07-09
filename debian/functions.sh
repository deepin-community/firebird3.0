# Shell functions library used by -{super,classic}.postinst
# This file needs to be sourced

if [ -z "${FB_VER:-}" ];
then
    echo Please define FB_VER before sourcing functions.sh
    exit 1
fi

export FB_VER
FB_VER_no_dots=$(echo "$FB_VER" | sed -e 's/\.//g')
SEC_API=$(echo "$FB_VER" | sed 's/\..*//')

FB="/usr/lib/firebird/$FB_VER"
VAR="/var/lib/firebird/$FB_VER"
ETC="/etc/firebird/$FB_VER"
LOG_DIR="/var/log/firebird"
LOG="$LOG_DIR/firebird${FB_VER}.log"
RUN="/var/run/firebird$FB_VER"
DBAPasswordFile="$ETC/SYSDBA.password"

create_var_run_firebird()
{
    if ! [ -d "$RUN" ]; then
        mkdir --parent "$RUN"
        chmod 0770 "$RUN"
        chown firebird:firebird "$RUN"
    fi
}

fixPerms() {
    create_var_run_firebird

    find "$VAR" -type d -exec chown firebird:firebird {} \; \
                           -exec chmod 0770 {} \;
    find "$VAR" -type f -exec chown firebird:firebird {} \; \
                           -exec chmod 0660 {} \;

    chmod 0770 "$LOG_DIR"
    chown firebird:firebird "$LOG_DIR"
}

runAsFirebird() {
    runuser -u firebird -g firebird -- /bin/sh -c "$*"
}

#---------------------------------------------------------------------------
# set new SYSDBA password with gsec

writeNewPassword () {
    local NewPassword=$1

    # Provide default SYSDBA.password
    if [ ! -e "$DBAPasswordFile" ];
    then
        touch "$DBAPasswordFile"
        chmod 0600 "$DBAPasswordFile"

        cat <<_EOF > "$DBAPasswordFile"
# Password for firebird SYSDBA user
#
# You may want to use the following command for changing it:
#   dpkg-reconfigure firebird${FB_VER}-server
#
# If you change the password manually with isql-fb or gsec, please update it
# here too. Keeping this file in sync with the security database is useful for
# any database maintenance scripts that need to connect as SYSDBA.

ISC_USER=sysdba
ISC_PASSWORD=
_EOF
    else
        . "$DBAPasswordFile"
    fi
    if [ "$NewPassword" != "${ISC_PASSWORD:-}" ]; then
        # SQL-quoted variant
        p=$(echo "$NewPassword" | sed "s/'/''/g")
        echo "create or alter user sysdba password '$p';" \
            | runAsFirebird isql-fb --fb-ver "$FB_VER" -user sysdba "$SEC_DB"

        # shell-quoted variant
        local sh_p=$(echo "$NewPassword" | sed 's/"/\\"/g')
        if grep "^ *ISC_PASSWORD=" "$DBAPasswordFile" > /dev/null;
        then
            # Update existing line

            # create .tmp file preserving permissions
            cp -a "$DBAPasswordFile" "$DBAPasswordFile.tmp"

            sed -e "s/^ *ISC_PASSWORD=.*/ISC_PASSWORD=\"$sh_p\"/" \
            < "$DBAPasswordFile" > "$DBAPasswordFile.tmp"
            mv -f "$DBAPasswordFile.tmp" "$DBAPasswordFile"
        else
            # Add new line
            echo "ISC_PASSWORD=\"$sh_p\"" >> "$DBAPasswordFile"
        fi

        ISC_PASSWORD=$NewPassword
    fi
}

askForDBAPassword ()
{
    if [ -f "$DBAPasswordFile" ];
    then
        . "$DBAPasswordFile"
    fi

    QUESTION=shared/firebird/sysdba_password/new_password

    db_get "$QUESTION" || true
    if [ -z "$RET" ];
    then
        if [ -z "${ISC_PASSWORD:-}" ];
        then
            NewPassword=$(cut -c 1-8 /proc/sys/kernel/random/uuid)
        else
            NewPassword=$ISC_PASSWORD
        fi
    else
        NewPassword=$RET
    fi

    writeNewPassword "$NewPassword"

    # Make debconf forget all password questions
    db_reset "$QUESTION" || true
    db_reset shared/firebird/sysdba_password/first_install || true
    db_reset shared/firebird/sysdba_password/upgrade_reconfigure || true
}

instantiate_security_db()
{
    SYS_DIR="$VAR/system"
    SEC_DB="$SYS_DIR/security$SEC_API.fdb"

    if ! [ -e "$SEC_DB" ];
    then
        local SEC_SQL="/usr/share/firebird/$FB_VER/security.sql"
        local T=$(mktemp -d)
        trap "rm -rf '$T'" 0 INT QUIT
        chmod 0700 "$T"
        chown firebird:firebird "$T"
        local T_SEC="$T/security.fdb"

        (
            set -e
            export LANG=C
            echo "create database '$T_SEC';" | runAsFirebird isql-fb --fb-ver "$FB_VER" -q
            runAsFirebird gfix --fb-ver "$FB_VER" -user SYSDBA -write async "$T_SEC"
            runAsFirebird isql-fb --fb-ver "$FB_VER" -user SYSDBA -i "$SEC_SQL" "$T_SEC"
        )
        runAsFirebird gfix --fb-ver "$FB_VER" -user SYSDBA -write sync "$T_SEC"
        install -o firebird -g firebird -m 0660 "$T_SEC" "$SEC_DB"

        # Since we've copied the default security database, the SYSDBA password
        # must be reset
        if [ -f "$DBAPasswordFile" ]; then
            rm "$DBAPasswordFile"
        fi
        echo "Created default $(basename "$SEC_DB")"
    fi
}

firebird_config_postinst()
{
    instantiate_security_db

    fixPerms

    askForDBAPassword

    debhelper_hook "$@"
}

# vi: set sw=4 ts=8 filetype=sh sts=4 :
